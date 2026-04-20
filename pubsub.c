// pubsub_driver.c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define DEVICE_NAME "pubsub"
#define MAX_TOPIC_NAME 64
#define DEFAULT_MAX_MSGS 5
#define DEFAULT_MAX_MSG_SIZE 250

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antonio Coelho, Francisco Freitas, Leonardo Andreucci e Pedro Dotti");
MODULE_DESCRIPTION("Pub/Sub kernel module (driver /dev/pubsub)");

static int max_msgs_per_sub = DEFAULT_MAX_MSGS;
static int max_msg_size = DEFAULT_MAX_MSG_SIZE;

module_param(max_msgs_per_sub, int, 0444);
MODULE_PARM_DESC(max_msgs_per_sub, "Max messages per subscriber per topic");
module_param(max_msg_size, int, 0444);
MODULE_PARM_DESC(max_msg_size, "Max size of each message (bytes)");

/* --- Data structures --- */

struct pub_msg {
    size_t len;
    char *data; // kmalloc'd, length max_msg_size
};

struct subscriber {
    pid_t pid;                     // process id (tgid)
    struct list_head list;         // link inside topic->subs
    struct mutex lock;             // protects this subscriber's queue
    struct pub_msg **queue;        // circular buffer of pointers to pub_msg
    int head, tail, count;         // circular buffer indices
    wait_queue_head_t wq;          // if you want blocking reads
};

struct topic {
    char name[MAX_TOPIC_NAME];
    struct list_head list;         // link inside global topics list
    struct mutex lock;             // protect subscribers list
    struct list_head subs;         // list_head of struct subscriber
};

/* global topics list */
static LIST_HEAD(topics);
static DEFINE_MUTEX(topics_lock);

/* char device infrastructure */
static dev_t dev_number;
static struct cdev pubsub_cdev;
static struct class *pubsub_class;

/* per-open file context */
struct pubsub_fctx {
    pid_t pid;
    struct topic *selected_topic;  // topic for subsequent reads
    char sel_topic_name[MAX_TOPIC_NAME];
};

static struct topic *find_topic_by_name(const char *name)
{
    struct topic *t;
    list_for_each_entry(t, &topics, list) {
        if (strncmp(t->name, name, MAX_TOPIC_NAME) == 0)
            return t;
    }
    return NULL;
}

static struct topic *get_or_create_topic(const char *name)
{
    struct topic *t;

    mutex_lock(&topics_lock);
    t = find_topic_by_name(name);
    if (t) {
        mutex_unlock(&topics_lock);
        return t;
    }

    t = kmalloc(sizeof(*t), GFP_KERNEL);
    if (!t) {
        mutex_unlock(&topics_lock);
        return NULL;
    }

    strncpy(t->name, name, MAX_TOPIC_NAME-1);
    t->name[MAX_TOPIC_NAME-1] = '\0';
    INIT_LIST_HEAD(&t->subs);
    mutex_init(&t->lock);
    list_add_tail(&t->list, &topics);
    mutex_unlock(&topics_lock);
    return t;
}

static void free_subscriber(struct subscriber *s)
{
    int i;
    if (!s) return;
    mutex_lock(&s->lock);
    for (i = 0; i < max_msgs_per_sub; ++i) {
        int idx = (s->head + i) % max_msgs_per_sub;
        if (s->queue[idx]) {
            kfree(s->queue[idx]->data);
            kfree(s->queue[idx]);
            s->queue[idx] = NULL;
        }
    }
    kfree(s->queue);
    mutex_unlock(&s->lock);
    kfree(s);
}

/* find subscriber by pid in topic; must be called with topic lock held or race can occur */
static struct subscriber *find_subscriber(struct topic *t, pid_t pid)
{
    struct subscriber *s;
    list_for_each_entry(s, &t->subs, list) {
        if (s->pid == pid)
            return s;
    }
    return NULL;
}

/* subscribe: add subscriber to topic */
static int topic_subscribe(struct topic *t, pid_t pid)
{
    struct subscriber *s;

    mutex_lock(&t->lock);
    s = find_subscriber(t, pid);
    if (s) {
        mutex_unlock(&t->lock);
        return 0; // already subscribed
    }

    s = kmalloc(sizeof(*s), GFP_KERNEL);
    if (!s) {
        mutex_unlock(&t->lock);
        return -ENOMEM;
    }
    s->pid = pid;
    mutex_init(&s->lock);
    s->queue = kmalloc_array(max_msgs_per_sub, sizeof(void *), GFP_KERNEL);
    if (!s->queue) {
        kfree(s);
        mutex_unlock(&t->lock);
        return -ENOMEM;
    }
    memset(s->queue, 0, max_msgs_per_sub * sizeof(void *));
    s->head = s->tail = s->count = 0;
    init_waitqueue_head(&s->wq);
    list_add_tail(&s->list, &t->subs);
    mutex_unlock(&t->lock);
    return 0;
}

/* unsubscribe: remove subscriber and free resources */
static int topic_unsubscribe(struct topic *t, pid_t pid)
{
    struct subscriber *s;
    int found = 0;

    mutex_lock(&t->lock);
    s = find_subscriber(t, pid);
    if (!s) {
        mutex_unlock(&t->lock);
        return -ENOENT;
    }
    list_del(&s->list);
    found = 1;
    mutex_unlock(&t->lock);

    if (found) {
        free_subscriber(s);
        return 0;
    }
    return -ENOENT;
}

/* enqueue a message into subscriber's queue; caller must not hold subscriber lock */
static int subscriber_enqueue(struct subscriber *s, const char *data, size_t len)
{
    struct pub_msg *m;
    unsigned long flags = 0;

    m = kmalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return -ENOMEM;
    m->data = kmalloc(max_msg_size, GFP_KERNEL);
    if (!m->data) {
        kfree(m);
        return -ENOMEM;
    }
    if (len > (size_t)max_msg_size) len = max_msg_size;
    memcpy(m->data, data, len);
    m->len = len;

    mutex_lock(&s->lock);
    if (s->count == max_msgs_per_sub) {
        /* drop oldest */
        struct pub_msg *old = s->queue[s->head];
        if (old) {
            kfree(old->data);
            kfree(old);
            s->queue[s->head] = NULL;
        }
        s->head = (s->head + 1) % max_msgs_per_sub;
        s->count--;
    }

    s->queue[s->tail] = m;
    s->tail = (s->tail + 1) % max_msgs_per_sub;
    s->count++;
    mutex_unlock(&s->lock);

    wake_up(&s->wq);
    return 0;
}

/* publish a message to the given topic */
static int topic_publish(struct topic *t, const char *data, size_t len)
{
    struct subscriber *s;
    int ret = 0;

    /* iterate subscribers */
    mutex_lock(&t->lock);
    list_for_each_entry(s, &t->subs, list) {
        /* for each subscriber, enqueue a copy */
        /* we do not hold subscriber lock across allocation (handled inside) */
        ret = subscriber_enqueue(s, data, len);
        if (ret) {
            /* if memory fails for one subscriber, continue other subscribers */
            ret = 0; // don't fail whole op
        }
    }
    mutex_unlock(&t->lock);
    return ret;
}

/* fetch: set file context selected topic */
static int fctx_fetch_topic(struct pubsub_fctx *fctx, const char *name)
{
    struct topic *t;

    t = find_topic_by_name(name);
    if (!t) return -ENOENT;

    /* store pointer; no refcount on topic, but topic won't be freed while still in global list.
       If you want to remove topics when empty, you'd need reference counting. Simpler: keep topics. */
    strlcpy(fctx->sel_topic_name, name, MAX_TOPIC_NAME);
    fctx->selected_topic = t;
    return 0;
}

/* read: remove one message from the subscriber queue for the selected topic and copy to user */
static ssize_t pubsub_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct pubsub_fctx *fctx = file->private_data;
    struct topic *t;
    struct subscriber *s;
    struct pub_msg *m;
    ssize_t tocopy;

    if (!fctx || !fctx->selected_topic)
        return -EINVAL;

    t = fctx->selected_topic;

    /* find our subscriber object for current pid */
    mutex_lock(&t->lock);
    s = find_subscriber(t, fctx->pid);
    if (!s) {
        mutex_unlock(&t->lock);
        return -ENOENT;
    }
    /* found subscriber; we'll manipulate its queue */
    /* To avoid long locks on topic, drop t->lock and use subscriber lock */
    mutex_unlock(&t->lock);

    mutex_lock(&s->lock);
    if (s->count == 0) {
        mutex_unlock(&s->lock);
        return 0; // null / no messages as spec
    }
    m = s->queue[s->head];
    if (!m) {
        /* inconsistent, but handle gracefully */
        s->head = (s->head + 1) % max_msgs_per_sub;
        s->count--;
        mutex_unlock(&s->lock);
        return 0;
    }
    /* determine how many bytes to copy */
    tocopy = min((size_t)m->len, count);
    if (copy_to_user(buf, m->data, tocopy)) {
        mutex_unlock(&s->lock);
        return -EFAULT;
    }
    /* free the message and advance head */
    kfree(m->data);
    kfree(m);
    s->queue[s->head] = NULL;
    s->head = (s->head + 1) % max_msgs_per_sub;
    s->count--;
    mutex_unlock(&s->lock);
    return tocopy;
}

/* write: parse commands */
static ssize_t pubsub_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char *kbuf;
    ssize_t ret = count;
    char cmd[16];
    char topic_name[MAX_TOPIC_NAME];
    char msg_buf[1024]; // temp local buffer for parsing; messages truncated to max_msg_size later
    int scanned;

    if (count == 0) return 0;
    if (count > 1023) count = 1023; // we only parse up to this
    kbuf = kmalloc(count+1, GFP_KERNEL);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    /* expect commands like:
       /subscribe topic1
       /unsubscribe topic1
       /publish topic1 "message..."
       /fetch topic1
    */
    if (sscanf(kbuf, "/%15s %63s %1023[^\n]", cmd, topic_name, msg_buf) >= 2) {
        pid_t pid = current->tgid;
        if (strcmp(cmd, "subscribe") == 0) {
            struct topic *t = get_or_create_topic(topic_name);
            if (!t) {
                ret = -ENOMEM;
                goto out;
            }
            topic_subscribe(t, pid);
            goto out;
        } else if (strcmp(cmd, "unsubscribe") == 0) {
            struct topic *t;
            mutex_lock(&topics_lock);
            t = find_topic_by_name(topic_name);
            mutex_unlock(&topics_lock);
            if (!t) {
                ret = -ENOENT;
                goto out;
            }
            topic_unsubscribe(t, pid);
            goto out;
        } else if (strcmp(cmd, "publish") == 0) {
            struct topic *t;
            char *payload = NULL;
            size_t payload_len = 0;

            /* msg_buf may include the leading space; alternatively parse string between quotes */
            /* Try to find first quote \" and next quote */
            char *p1 = strchr(kbuf, '"');
            char *p2 = NULL;
            if (p1) {
                p2 = strchr(p1+1, '"');
                if (p2) {
                    payload_len = p2 - (p1+1);
                    if (payload_len > (size_t)max_msg_size)
                        payload_len = max_msg_size;
                    payload = kmalloc(payload_len+1, GFP_KERNEL);
                    if (!payload) {
                        ret = -ENOMEM;
                        goto out;
                    }
                    memcpy(payload, p1+1, payload_len);
                    payload[payload_len] = '\0';
                } else {
                    /* no closing quote: take what's after topic */
                    char *after = strchr(kbuf, ' ');
                    if (after) {
                        after = strchr(after+1, ' ');
                        if (after) {
                            size_t l = strlen(after+1);
                            payload_len = min(l, (size_t)max_msg_size);
                            payload = kmalloc(payload_len+1, GFP_KERNEL);
                            if (!payload) {
                                ret = -ENOMEM;
                                goto out;
                            }
                            memcpy(payload, after+1, payload_len);
                            payload[payload_len] = '\0';
                        }
                    }
                }
            }

            mutex_lock(&topics_lock);
            t = find_topic_by_name(topic_name);
            mutex_unlock(&topics_lock);
            if (!t) {
                /* If no such topic, nothing to do; or optionally create topic */
                /* create it so future subscribers can exist or do nothing */
                t = get_or_create_topic(topic_name);
                if (!t) {
                    kfree(payload);
                    ret = -ENOMEM;
                    goto out;
                }
            }
            if (!payload) {
                /* nothing to publish */
                goto out;
            }
            topic_publish(t, payload, payload_len);
            kfree(payload);
            goto out;
        } else if (strcmp(cmd, "fetch") == 0) {
            struct pubsub_fctx *fctx = file->private_data;
            if (!fctx) {
                ret = -EINVAL;
                goto out;
            }
            if (fctx_fetch_topic(fctx, topic_name) < 0) {
                ret = -ENOENT;
                goto out;
            }
            goto out;
        }
    }

    /* unknown command */
    ret = -EINVAL;

out:
    kfree(kbuf);
    return ret;
}

/* open: allocate file context */
static int pubsub_open(struct inode *inode, struct file *file)
{
    struct pubsub_fctx *fctx;
    fctx = kmalloc(sizeof(*fctx), GFP_KERNEL);
    if (!fctx) return -ENOMEM;
    fctx->pid = current->tgid;
    fctx->selected_topic = NULL;
    fctx->sel_topic_name[0] = '\0';
    file->private_data = fctx;
    return 0;
}

/* release: cleanup subscriptions for this pid across all topics? 
   According to spec, user may subscribe to topics by writing /subscribe; unlocking resources must remove subscriptions made by this pid.
*/
static int pubsub_release(struct inode *inode, struct file *file)
{
    struct pubsub_fctx *fctx = file->private_data;
    struct topic *t, *tmp;
    pid_t pid = current->tgid;

    /* remove this pid from all topics (unsubscribe) */
    mutex_lock(&topics_lock);
    list_for_each_entry_safe(t, tmp, &topics, list) {
        /* remove subscriber if exists */
        mutex_lock(&t->lock);
        {
            struct subscriber *s = find_subscriber(t, pid);
            if (s) {
                list_del(&s->list);
                /* free subscriber outside topic lock */
                mutex_unlock(&t->lock);
                free_subscriber(s);
                mutex_lock(&t->lock);
            }
        }
        mutex_unlock(&t->lock);
    }
    mutex_unlock(&topics_lock);

    if (fctx) kfree(fctx);
    file->private_data = NULL;
    return 0;
}

static const struct file_operations pubsub_fops = {
    .owner = THIS_MODULE,
    .open = pubsub_open,
    .release = pubsub_release,
    .read = pubsub_read,
    .write = pubsub_write,
};

static int __init pubsub_init(void)
{
    int err;

    /* allocate device number */
    err = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME);
    if (err < 0) {
        pr_err("pubsub: failed to alloc dev region\n");
        return err;
    }

    cdev_init(&pubsub_cdev, &pubsub_fops);
    pubsub_cdev.owner = THIS_MODULE;
    err = cdev_add(&pubsub_cdev, dev_number, 1);
    if (err) {
        unregister_chrdev_region(dev_number, 1);
        pr_err("pubsub: cdev_add failed\n");
        return err;
    }

    pubsub_class = class_create(THIS_MODULE, "pubsub_class");
    if (IS_ERR(pubsub_class)) {
        cdev_del(&pubsub_cdev);
        unregister_chrdev_region(dev_number, 1);
        return PTR_ERR(pubsub_class);
    }

    if (device_create(pubsub_class, NULL, dev_number, NULL, DEVICE_NAME) == NULL) {
        class_destroy(pubsub_class);
        cdev_del(&pubsub_cdev);
        unregister_chrdev_region(dev_number, 1);
        return -ENOMEM;
    }

    pr_info("pubsub: loaded (max_msgs=%d max_msg_size=%d)\n", max_msgs_per_sub, max_msg_size);
    return 0;
}

static void cleanup_topics(void)
{
    struct topic *t, *tmp;
    mutex_lock(&topics_lock);
    list_for_each_entry_safe(t, tmp, &topics, list) {
        struct subscriber *s, *s_tmp;
        /* remove from list */
        list_del(&t->list);
        mutex_unlock(&topics_lock);

        /* free subscribers */
        mutex_lock(&t->lock);
        list_for_each_entry_safe(s, s_tmp, &t->subs, list) {
            list_del(&s->list);
            mutex_unlock(&t->lock);
            free_subscriber(s);
            mutex_lock(&t->lock);
        }
        mutex_unlock(&t->lock);

        kfree(t);
        mutex_lock(&topics_lock);
    }
    mutex_unlock(&topics_lock);
}

static void __exit pubsub_exit(void)
{
    device_destroy(pubsub_class, dev_number);
    class_destroy(pubsub_class);
    cdev_del(&pubsub_cdev);
    unregister_chrdev_region(dev_number, 1);
    cleanup_topics();
    pr_info("pubsub: unloaded\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);
