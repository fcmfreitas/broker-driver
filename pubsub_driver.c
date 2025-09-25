// pubsub_driver.c (versão C90-compatível para eliminar warnings)

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h> // Para 'current'

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antonio Coelho e Francisco Freitas e Leonardo Andreucci e Pedro Dotti");
MODULE_DESCRIPTION("Broker Publish/Subscribe via Device Driver");

#define DEVICE_NAME "pubsub"
#define CLASS_NAME  "pubsub_class"

// Parâmetros do módulo
static int max_msgs = 10;
static int max_msg_size = 256;

module_param(max_msgs, int, S_IRUGO);
MODULE_PARM_DESC(max_msgs, "Numero maximo de mensagens por topico/processo");
module_param(max_msg_size, int, S_IRUGO);
MODULE_PARM_DESC(max_msg_size, "Tamanho maximo de cada mensagem em bytes");

// Variáveis globais
static int major_number;
static struct class* pubsub_class = NULL;
static struct device* pubsub_device = NULL;
// A variável pubsub_cdev não era usada, então foi removida para eliminar o aviso.

static DEFINE_MUTEX(topic_mutex);

// --- Estruturas de Dados ---
struct message {
    char *data;
    struct list_head list;
};
struct subscription {
    pid_t pid;
    int msg_count;
    struct list_head messages;
    struct list_head list;
};
struct topic {
    char *name;
    struct list_head subscriptions;
    struct list_head list;
};
struct read_context {
    struct subscription *sub;
};

static LIST_HEAD(topic_list);

// --- Funções Auxiliares ---
static struct topic* find_topic(const char* name) {
    struct topic *t;
    list_for_each_entry(t, &topic_list, list) {
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}
static struct subscription* find_subscription(struct topic* t, pid_t pid) {
    struct subscription *s;
    list_for_each_entry(s, &t->subscriptions, list) {
        if (s->pid == pid) return s;
    }
    return NULL;
}
static void free_subscription(struct subscription *sub) {
    struct message *msg, *tmp_msg;
    list_for_each_entry_safe(msg, tmp_msg, &sub->messages, list) {
        list_del(&msg->list);
        kfree(msg->data);
        kfree(msg);
    }
    kfree(sub);
}

// --- Funções de File Operations ---
static int pubsub_open(struct inode *inodep, struct file *filep) {
    struct read_context *rctx = kmalloc(sizeof(struct read_context), GFP_KERNEL);
    if (!rctx) return -ENOMEM;
    rctx->sub = NULL;
    filep->private_data = rctx;
    printk(KERN_INFO "PubSub: Dispositivo aberto por PID %d\n", current->pid);
    return 0;
}
static int pubsub_release(struct inode *inodep, struct file *filep) {
    if (filep->private_data) kfree(filep->private_data);
    printk(KERN_INFO "PubSub: Dispositivo fechado por PID %d\n", current->pid);
    return 0;
}
static ssize_t pubsub_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    struct read_context *rctx = filep->private_data;
    struct subscription *sub;
    struct message *msg;
    ssize_t msg_len;

    if (!rctx || !rctx->sub) return -EINVAL;
    
    sub = rctx->sub;
    mutex_lock(&topic_mutex);
    if (list_empty(&sub->messages)) {
        mutex_unlock(&topic_mutex);
        return 0;
    }
    msg = list_first_entry(&sub->messages, struct message, list);
    msg_len = strlen(msg->data);
    if (copy_to_user(buffer, msg->data, msg_len)) {
        mutex_unlock(&topic_mutex);
        return -EFAULT;
    }
    list_del(&msg->list);
    sub->msg_count--;
    kfree(msg->data);
    kfree(msg);
    mutex_unlock(&topic_mutex);
    return msg_len;
}
static ssize_t pubsub_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    char *kernel_buffer, *cmd, *topic_name, *msg_body;
    struct read_context *rctx = filep->private_data;
    struct topic *t;
    struct subscription *s;

    if (len == 0) return 0;
    kernel_buffer = kmalloc(len + 1, GFP_KERNEL);
    if (!kernel_buffer) return -ENOMEM;
    if (copy_from_user(kernel_buffer, buffer, len)) {
        kfree(kernel_buffer);
        return -EFAULT;
    }
    kernel_buffer[len] = '\0';
    cmd = strsep(&kernel_buffer, " ");
    mutex_lock(&topic_mutex);

    if (strcmp(cmd, "/subscribe") == 0) {
        topic_name = strsep(&kernel_buffer, " \n");
        if (topic_name) {
            t = find_topic(topic_name);
            if (!t) {
                t = kmalloc(sizeof(struct topic), GFP_KERNEL);
                t->name = kstrdup(topic_name, GFP_KERNEL);
                INIT_LIST_HEAD(&t->subscriptions);
                list_add_tail(&t->list, &topic_list);
            }
            if (!find_subscription(t, current->pid)) {
                s = kmalloc(sizeof(struct subscription), GFP_KERNEL);
                s->pid = current->pid;
                s->msg_count = 0;
                INIT_LIST_HEAD(&s->messages);
                list_add_tail(&s->list, &t->subscriptions);
                printk(KERN_INFO "PubSub: PID %d inscrito no topico '%s'.\n", current->pid, topic_name);
            }
        }
    } else if (strcmp(cmd, "/unsubscribe") == 0) {
        topic_name = strsep(&kernel_buffer, " \n");
        if (topic_name) {
            t = find_topic(topic_name);
            if (t) {
                s = find_subscription(t, current->pid);
                if (s) {
                    if (!list_empty(&s->messages)) printk(KERN_WARNING "PubSub: PID %d cancelou inscricao no topico '%s' com %d mensagens nao lidas.\n", current->pid, topic_name, s->m;
                    list_del(&s->list);
                    free_subscription(s);
                    printk(KERN_INFO "PubSub: PID %d cancelou inscricao do topico '%s'.\n", current->pid, topic_name);
                }
            }
        }
    } else if (strcmp(cmd, "/publish") == 0) {
        topic_name = strsep(&kernel_buffer, " ");
        msg_body = kernel_buffer;
        if (topic_name && msg_body) {
            if (msg_body[0] == '\"') {
                msg_body++;
                msg_body[strlen(msg_body) - 1] = '\0';
            }
            if (strlen(msg_body) > max_msg_size) {
                printk(KERN_WARNING "PubSub: Mensagem para o topico '%s' maior que %d bytes e foi descartada.\n", topic_name, max_msg_size);
            } else {
                t = find_topic(topic_name);
                if (t) {
                    list_for_each_entry(s, &t->subscriptions, list) {
                        struct message *new_msg; // Declaração movida
                        if (s->msg_count >= max_msgs) {
                            struct message *old_msg = list_first_entry(&s->messages, struct message, list);
                            list_del(&old_msg->list);
                            kfree(old_msg->data);
                            kfree(old_msg);
                            s->msg_count--;
                            printk(KERN_WARNING "PubSub: Fila cheia para PID %d no topico '%s'. Mensagem antiga descartada.\n", s->pid, topic_name);
                        }
                        new_msg = kmalloc(sizeof(struct message), GFP_KERNEL);
                        new_msg->data = kstrdup(msg_body, GFP_KERNEL);
                        list_add_tail(&new_msg->list, &s->messages);
                        s->msg_count++;
                    }
                    printk(KERN_INFO "PubSub: Mensagem publicada no topico '%s'.\n", topic_name);
                }
            }
        }
    } else if (strcmp(cmd, "/fetch") == 0) {
        topic_name = strsep(&kernel_buffer, " \n");
        if (topic_name) {
            t = find_topic(topic_name);
            rctx->sub = t ? find_subscription(t, current->pid) : NULL;
            if (rctx->sub) printk(KERN_INFO "PubSub: PID %d pronto para ler do topico '%s'.\n", current->pid, topic_name);
        }
    } else {
        printk(KERN_WARNING "PubSub: Comando desconhecido: %s\n", cmd);
    }
    mutex_unlock(&topic_mutex);
    kfree(cmd);
    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = pubsub_open,
    .release = pubsub_release,
    .read = pubsub_read,
    .write = pubsub_write,
};

// --- Funções de Inicialização e Saída do Módulo ---
static int __init pubsub_init(void) {
    printk(KERN_INFO "PubSub: Carregando modulo...\n");
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;
    pubsub_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(pubsub_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(pubsub_class);
    }
    pubsub_device = device_create(pubsub_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(pubsub_device)) {
        class_destroy(pubsub_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(pubsub_device);
    }
    printk(KERN_INFO "PubSub: Modulo carregado. Major: %d\n", major_number);
    return 0;
}
static void __exit pubsub_exit(void) {
    struct topic *t, *tmp_t;
    struct subscription *s, *tmp_s;
    mutex_lock(&topic_mutex);
    list_for_each_entry_safe(t, tmp_t, &topic_list, list) {
        list_for_each_entry_safe(s, tmp_s, &t->subscriptions, list) {
            list_del(&s->list);
            free_subscription(s);
        }
        list_del(&t->list);
        kfree(t->name);
        kfree(t);
    }
    mutex_unlock(&topic_mutex);
    mutex_destroy(&topic_mutex);
    device_destroy(pubsub_class, MKDEV(major_number, 0));
    class_unregister(pubsub_class);
    class_destroy(pubsub_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "PubSub: Modulo descarregado.\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);
