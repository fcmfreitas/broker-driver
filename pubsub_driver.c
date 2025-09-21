#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h> // kmalloc e kfree
#include <linux/list.h> // API de listas do Kernel
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Francisco Freitas");
MODULE_DESCRIPTION("Broker Publish/Subscribe via Device Driver");

#define DEVICE_NAME "pubsub"
#define CLASS_NAME  "pubsub_class"

// Variáveis dos parâmetros do módulo
static int max_msgs = 10;
static int max_msg_size = 256;

module_param(max_msgs, int, S_IRUGO);
MODULE_PARM_DESC(max_msgs, "Numero maximo de mensagens por topico/processo");
module_param(max_msg_size, int, S_IRUGO);
MODULE_PARM_DESC(max_msg_size, "Tamanho maximo de cada mensagem em bytes");

// Variáveis globais para o device driver
static int major_number;
static struct class* pubsub_class = NULL;
static struct device* pubsub_device = NULL;
static struct cdev pubsub_cdev;

// Mutex para proteger o acesso concorrente
static DEFINE_MUTEX(topic_mutex);

// TODO: Definir suas estruturas de dados aqui
// Exemplo:
//
// struct message {
//     char *data;
//     struct list_head list;
// };
//
// struct subscription {
//     pid_t pid;
//     struct list_head messages; // Fila de 'struct message'
//     struct list_head list;
// };
//
// struct topic {
//     char *name;
//     struct list_head subscriptions; // Lista de 'struct subscription'
//     struct list_head list;
// };
//
// // Cabeça da lista global de tópicos
// static LIST_HEAD(topic_list);


// --- Funções File Operations ---

static int pubsub_open(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "PubSub: Dispositivo aberto por PID %d\n", current->pid);
    // filep->private_data pode ser usado para armazenar dados específicos desta instância de 'open'
    return 0;
}

static int pubsub_release(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "PubSub: Dispositivo fechado por PID %d\n", current->pid);
    // TODO: Limpar recursos associados a este processo se ele não se desinscreveu.
    // Por exemplo, remover todas as suas inscrições.
    return 0;
}

static ssize_t pubsub_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    // Lógica de leitura de mensagens
    // 1. Verificar se um tópico foi selecionado com /fetch (usando filep->private_data)
    // 2. Acessar a fila de mensagens do processo/tópico correto
    // 3. Retirar a mensagem mais antiga da fila
    // 4. Copiar a mensagem para o buffer do usuário com copy_to_user()
    // 5. Liberar a memória da mensagem com kfree()
    // 6. Retornar o número de bytes lidos ou 0 se não houver mensagens.
    printk(KERN_INFO "PubSub: Operacao de read() chamada.\n");

    // Exemplo de placeholder:
    char msg[] = "Nenhuma mensagem\n";
    int msg_len = strlen(msg);

    if (*offset > 0) {
        return 0; // Sinaliza fim da leitura
    }

    if (copy_to_user(buffer, msg, msg_len)) {
        return -EFAULT;
    }
    *offset = msg_len;
    return msg_len;
}

static ssize_t pubsub_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    char *kernel_buffer;

    if (len > max_msg_size + 100) { // Adiciona um buffer para o comando
        printk(KERN_WARNING "PubSub: Comando muito longo descartado.\n");
        return -EINVAL;
    }

    kernel_buffer = kmalloc(len + 1, GFP_KERNEL);
    if (!kernel_buffer) {
        return -ENOMEM;
    }

    if (copy_from_user(kernel_buffer, buffer, len)) {
        kfree(kernel_buffer);
        return -EFAULT;
    }
    kernel_buffer[len] = '\0';

    mutex_lock(&topic_mutex);

    // --- Lógica de Parsing e Execução de Comandos ---
    if (strncmp(kernel_buffer, "/subscribe", 10) == 0) {
        // TODO: Implementar lógica de inscrição
        // 1. Extrair o nome do tópico
        // 2. Encontrar o tópico na lista global ou criar um novo
        // 3. Adicionar o PID do processo atual (current->pid) à lista de inscrições do tópico
        printk(KERN_INFO "PubSub: Comando SUBSCRIBE recebido: %s", kernel_buffer);

    } else if (strncmp(kernel_buffer, "/unsubscribe", 12) == 0) {
        // TODO: Implementar lógica de desinscrição
        // 1. Extrair o nome do tópico
        // 2. Encontrar o tópico e a inscrição do PID
        // 3. Remover a inscrição e liberar mensagens pendentes (gerando alerta )
        printk(KERN_INFO "PubSub: Comando UNSUBSCRIBE recebido: %s", kernel_buffer);

    } else if (strncmp(kernel_buffer, "/publish", 8) == 0) {
        // TODO: Implementar lógica de publicação
        // 1. Extrair nome do tópico e mensagem
        // 2. Encontrar o tópico
        // 3. Para cada processo inscrito, enfileirar uma CÓPIA da mensagem
        // 4. Lidar com fila cheia (descartar mais antiga )
        printk(KERN_INFO "PubSub: Comando PUBLISH recebido: %s", kernel_buffer);

    } else if (strncmp(kernel_buffer, "/fetch", 6) == 0) {
        // TODO: Implementar lógica de fetch
        // 1. Extrair nome do tópico
        // 2. Encontrar a inscrição do PID atual no tópico
        // 3. Armazenar um ponteiro para a inscrição em 'filep->private_data'
        //    Isso criará um "contexto" para as próximas chamadas de read().
        printk(KERN_INFO "PubSub: Comando FETCH recebido: %s", kernel_buffer);

    } else {
        printk(KERN_WARNING "PubSub: Comando desconhecido: %s\n", kernel_buffer);
    }

    mutex_unlock(&topic_mutex);
    kfree(kernel_buffer);
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
    printk(KERN_INFO "PubSub: Parametros: max_msgs=%d, max_msg_size=%d\n", max_msgs, max_msg_size);

    // 1. Alocar Major Number
    if (alloc_chrdev_region(&major_number, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ALERT "PubSub: Falha ao alocar major number\n");
        return -1;
    }
    major_number = MAJOR(major_number);
    printk(KERN_INFO "PubSub: Major number alocado: %d\n", major_number);

    // 2. Criar a classe do dispositivo
    pubsub_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(pubsub_class)) {
        unregister_chrdev_region(MKDEV(major_number, 0), 1);
        printk(KERN_ALERT "PubSub: Falha ao registrar a classe do dispositivo\n");
        return PTR_ERR(pubsub_class);
    }

    // 3. Criar o dispositivo
    pubsub_device = device_create(pubsub_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(pubsub_device)) {
        class_destroy(pubsub_class);
        unregister_chrdev_region(MKDEV(major_number, 0), 1);
        printk(KERN_ALERT "PubSub: Falha ao criar o dispositivo\n");
        return PTR_ERR(pubsub_device);
    }

    // 4. Inicializar e adicionar o cdev
    cdev_init(&pubsub_cdev, &fops);
    if (cdev_add(&pubsub_cdev, MKDEV(major_number, 0), 1) < 0) {
        device_destroy(pubsub_class, MKDEV(major_number, 0));
        class_destroy(pubsub_class);
        unregister_chrdev_region(MKDEV(major_number, 0), 1);
        printk(KERN_ALERT "PubSub: Falha ao adicionar o cdev\n");
        return -1;
    }

    printk(KERN_INFO "PubSub: Modulo carregado com sucesso!\n");
    return 0;
}

static void __exit pubsub_exit(void) {
    // TODO: Adicionar lógica para liberar TODA a memória alocada (tópicos, inscrições, mensagens)
    // Isso é CRÍTICO para evitar memory leaks no kernel. 

    mutex_destroy(&topic_mutex);
    device_destroy(pubsub_class, MKDEV(major_number, 0));
    class_unregister(pubsub_class);
    class_destroy(pubsub_class);
    unregister_chrdev_region(MKDEV(major_number, 0), 1);
    cdev_del(&pubsub_cdev);
    printk(KERN_INFO "PubSub: Modulo descarregado.\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);
