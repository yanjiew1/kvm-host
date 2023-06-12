#include <fcntl.h>
#include <linux/serial_reg.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "err.h"
#include "serial.h"
#include "utils.h"
#include "vm.h"

#define SERIAL_IRQ 4
#define IO_READ8(data) *((uint8_t *) data)
#define IO_WRITE8(data, value) ((uint8_t *) data)[0] = value

#define IER_MASK 0x0f
#define MCR_MASK 0x3f
#define FCR_MASK 0xe9

struct serial_dev_priv {
    /* Device registers */
    uint8_t dll;
    uint8_t dlm;
    uint8_t iir;
    uint8_t ier;
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
    bool thr_ipending;

    /* Buffers */
    struct fifo tx_buf;
    struct fifo rx_buf;

    /* File descriptors */
    int infd;
    int outfd;
    int evfd;
    int epollfd;

    /* Worker */
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_mutex_t loopback_lock;
    volatile bool stop;

    /* Initialized */
    bool initialized;
};

static struct serial_dev_priv serial_dev_priv = {
    .iir = UART_IIR_NO_INT,
    .mcr = UART_MCR_OUT2,
    .lsr = UART_LSR_TEMT | UART_LSR_THRE,
    .msr = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,
};

static void serial_signal(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint64_t buf = 1;
    while (write(priv->evfd, &buf, 8) < 0)
        ;
}

/* FIXME: This implementation is incomplete */
static void serial_update_irq(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t iir = UART_IIR_NO_INT;

    /* If enable receiver data interrupt and receiver data ready */
    if ((priv->ier & UART_IER_RDI) && (priv->lsr & UART_LSR_DR))
        iir = UART_IIR_RDI;
    /* If enable transmiter data interrupt and transmiter empty */
    else if ((priv->ier & UART_IER_THRI) && (priv->lsr & UART_LSR_THRE) &&
             priv->thr_ipending)
        iir = UART_IIR_THRI;

    priv->iir = iir;
    if (priv->fcr & UART_FCR_ENABLE_FIFO) {
        priv->iir |= 0xc0;
        if ((priv->lcr & UART_LCR_DLAB) && (priv->fcr & UART_FCR7_64BYTE))
            priv->iir |= 0x20;
    }

    /* FIXME: the return error of vm_irq_line should be handled */
    vm_irq_line(container_of(s, vm_t, serial), SERIAL_IRQ,
                iir == UART_IIR_NO_INT ? 0 /* inactive */ : 1 /* active */);
}

#define FREQ_NS ((int) (1.0e6))
#define NS_PER_SEC ((int) (1.0e9))

static void serial_receive(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    struct iovec iov[2];
    int iovc;
    if (fifo_is_full(&priv->rx_buf))
        return;

    int head = (priv->rx_buf.head - 1) % FIFO_LEN + 1;
    int tail = priv->rx_buf.tail % FIFO_LEN;
    uint8_t *buf = priv->rx_buf.data;
    int len;

    if (tail < head) {
        iov[0].iov_base = &buf[tail];
        iov[0].iov_len = head - tail;
        iovc = 1;
    } else {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = FIFO_LEN - tail;
        iov[1].iov_base = buf;
        iov[1].iov_len = head;
        iovc = 2;
    }
    len = readv(priv->infd, iov, iovc);
    if (len > 0)
        priv->rx_buf.tail += len;
    pthread_mutex_lock(&priv->lock);
    if (!(priv->lsr & UART_LSR_DR) && !fifo_is_empty(&priv->rx_buf)) {
        priv->lsr |= UART_LSR_DR;
        serial_update_irq(s);
    }
    pthread_mutex_unlock(&priv->lock);
}

static void serial_transmit(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    struct iovec iov[2];
    int iovc;
    if (fifo_is_empty(&priv->tx_buf))
        return;

    int head = priv->tx_buf.head % FIFO_LEN;
    int tail = (priv->tx_buf.tail - 1) % FIFO_LEN + 1;
    uint8_t *buf = priv->tx_buf.data;
    int len;

    if (head < tail) {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = tail - head;
        iovc = 1;
    } else {
        iov[0].iov_base = &buf[head];
        iov[0].iov_len = FIFO_LEN - head;
        iov[1].iov_base = buf;
        iov[1].iov_len = tail;
        iovc = 2;
    }
    len = writev(priv->outfd, iov, iovc);
    if (len > 0)
        priv->tx_buf.head += len;
    if (fifo_is_empty(&priv->tx_buf)) {
        pthread_mutex_lock(&priv->lock);
        if (!(priv->lsr & UART_LSR_THRE) && fifo_is_empty(&priv->tx_buf)) {
            priv->lsr |= UART_LSR_THRE | UART_LSR_TEMT;
            priv->thr_ipending = true;
            serial_update_irq(s);
        }
        pthread_mutex_unlock(&priv->lock);
    }
}

static void serial_loopback(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t tmp;

    while (!fifo_is_empty(&priv->tx_buf) && !fifo_is_full(&priv->rx_buf)) {
        if (!fifo_get(&priv->tx_buf, tmp))
            break;
        fifo_put(&priv->rx_buf, tmp);
    }

    if (fifo_is_empty(&priv->tx_buf)) {
        priv->lsr |= UART_LSR_TEMT | UART_LSR_THRE;        
    }

    if (!fifo_is_empty(&priv->rx_buf)) {
        priv->lsr |= UART_LSR_DR;
    }

    serial_update_irq(s);
}

static void *serial_thread(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    int epollfd = priv->epollfd;
    struct epoll_event event;
    uint64_t tmp;

    while (!__atomic_load_n(&priv->stop, __ATOMIC_RELAXED)) {
        int ret = epoll_wait(epollfd, &event, 1, -1);
        if (ret < 1)
            continue;
        if (event.data.u64)
            while (read(priv->evfd, &tmp, 8) < 0)
                ;
        pthread_mutex_lock(&priv->loopback_lock);
        serial_transmit(s);
        serial_receive(s);
        pthread_mutex_unlock(&priv->loopback_lock);
    }
    return NULL;
}

static void serial_in(serial_dev_t *s, uint16_t offset, void *data)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;

    switch (offset) {
    case UART_RX:
        if (priv->lcr & UART_LCR_DLAB) {
            IO_WRITE8(data, priv->dll);
        } else {
            if (fifo_is_empty(&priv->rx_buf)) {
                IO_WRITE8(data, 0);
                break;
            }

            uint8_t value;
            if (fifo_get(&priv->rx_buf, value))
                IO_WRITE8(data, value);
            int level = fifo_level(&priv->rx_buf);
            if (level == 0) {
                pthread_mutex_lock(&priv->lock);
                /* check again the fifo level before modify the register */
                level = fifo_level(&priv->rx_buf);
                if (level == 0) {
                    priv->lsr &= ~UART_LSR_DR;
                    serial_update_irq(s);
                }
                pthread_mutex_unlock(&priv->lock);
            }
            if (level == FIFO_LEN - 1)
                serial_signal(s);
        }
        break;
    case UART_IER:
        if (priv->lcr & UART_LCR_DLAB)
            IO_WRITE8(data, priv->dlm);
        else
            IO_WRITE8(data, priv->ier);
        break;
    case UART_IIR:
        IO_WRITE8(data, priv->iir);
        if ((priv->iir & UART_IIR_ID) == UART_IIR_THRI) {
            pthread_mutex_lock(&priv->lock);
            priv->thr_ipending = false;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        }
        break;
    case UART_LCR:
        IO_WRITE8(data, priv->lcr);
        break;
    case UART_MCR:
        IO_WRITE8(data, priv->mcr);
        break;
    case UART_LSR:
        IO_WRITE8(data, priv->lsr);
        /* clear error bits */
        pthread_mutex_lock(&priv->lock);
        priv->lsr &= ~UART_LSR_BRK_ERROR_BITS;
        serial_update_irq(s);
        pthread_mutex_unlock(&priv->lock);
        break;
    case UART_MSR:
        IO_WRITE8(data, priv->msr);
        break;
    case UART_SCR:
        IO_WRITE8(data, priv->scr);
        break;
    default:
        break;
    }
}

static void serial_out(serial_dev_t *s, uint16_t offset, void *data)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t orig, value = IO_READ8(data);
    switch (offset) {
    case UART_TX:
        if (priv->lcr & UART_LCR_DLAB) {
            priv->dll = IO_READ8(data);
            break;
        }
        if (fifo_is_full(&priv->tx_buf))
            break;

        fifo_put(&priv->tx_buf, value);

        if (priv->mcr & UART_MCR_LOOP) {
            /* Loopback mode, drain tx */
            serial_loopback(s);
            break;
        }

        int level = fifo_level(&priv->tx_buf);
        if (level == 1) {
            pthread_mutex_lock(&priv->lock);
            /* check again the fifo level before modify the register */
            level = fifo_level(&priv->tx_buf);
            if (level == 1) {
                priv->lsr &= ~(UART_LSR_TEMT | UART_LSR_THRE);
                serial_update_irq(s);
            }
            pthread_mutex_unlock(&priv->lock);
        }
        if (level == 1)
            serial_signal(s);

        break;
    case UART_IER:
        if (!(priv->lcr & UART_LCR_DLAB)) {
            pthread_mutex_lock(&priv->lock);
            priv->ier = IO_READ8(data) & IER_MASK;
            serial_update_irq(s);
            pthread_mutex_unlock(&priv->lock);
        } else {
            priv->dlm = IO_READ8(data);
        }
        break;
    case UART_FCR:
        value = IO_READ8(data);
        pthread_mutex_lock(&priv->lock);
        priv->fcr = IO_READ8(data) & FCR_MASK;
        if (value & (UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT)) {
            if (value & UART_FCR_CLEAR_RCVR) {
                fifo_clear(&priv->rx_buf);
                priv->lsr &= ~UART_LSR_DR;
            }
            if (value & UART_FCR_CLEAR_XMIT) {
                fifo_clear(&priv->tx_buf);
                priv->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
                priv->thr_ipending = true;
            }
        }
        serial_update_irq(s);
        pthread_mutex_unlock(&priv->lock);
        break;
    case UART_LCR:
        priv->lcr = IO_READ8(data);
        pthread_mutex_lock(&priv->lock);
        serial_update_irq(s);
        pthread_mutex_unlock(&priv->lock);
        break;
    case UART_MCR:
        orig = priv->mcr;
        value = IO_READ8(data);
        priv->mcr = IO_READ8(data) & MCR_MASK;
        if ((orig & UART_MCR_LOOP) && !(value & UART_MCR_LOOP)) {
            serial_loopback(s);
            pthread_mutex_unlock(&priv->loopback_lock);
        }
        if (!(orig & UART_MCR_LOOP) && (value & UART_MCR_LOOP)) {
            pthread_mutex_lock(&priv->loopback_lock);
            serial_loopback(s);
        }
        break;
    case UART_LSR: /* factory test */
    case UART_MSR: /* not used */
        break;
    case UART_SCR:
        priv->scr = IO_READ8(data);
        break;
    default:
        break;
    }
}

static void serial_handle_io(void *owner,
                             void *data,
                             uint8_t is_write,
                             uint64_t offset,
                             uint8_t size)
{
    serial_dev_t *s = (serial_dev_t *) owner;
    void (*serial_op)(serial_dev_t *, uint16_t, void *) =
        is_write ? serial_out : serial_in;

    serial_op(s, offset, data);
}

int serial_init(serial_dev_t *s, struct bus *bus)
{
    struct serial_dev_priv *priv = &serial_dev_priv;
    struct epoll_event event;

    if (priv->initialized)
        return throw_err("Serial device is already initialized\n");

    s->priv = &serial_dev_priv;

    /* Create necessory file descriptors */
    int evfd, infd, outfd, epollfd;
    evfd = infd = outfd = epollfd = -1;

    evfd = eventfd(0, EFD_NONBLOCK);
    if (evfd < 0) {
        throw_err("Failed to create eventfd\n");
        goto err;
    }
    infd = open("/dev/stdin", O_RDONLY | O_NONBLOCK);
    if (infd < 0) {
        throw_err("Failed to open stdin device\n");
        goto err;
    }
    outfd = open("/dev/stdout", O_WRONLY | O_NONBLOCK);
    if (outfd < 0) {
        throw_err("Failed to open stdout device\n");
        goto err;
    }
    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        throw_err("Failed to create epoll file descriptor\n");
        goto err;
    }

    priv->evfd = evfd;
    priv->infd = infd;
    priv->outfd = outfd;
    priv->epollfd = epollfd;

    /* Setup epoll */
    event.events = EPOLLIN;
    event.data.u64 = 1;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, evfd, &event) < 0) {
        throw_err("Failed to add eventfd to epoll\n");
        goto err;
    }

    event.events = EPOLLIN | EPOLLET;
    event.data.u64 = 0;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, infd, &event) < 0) {
        throw_err("Failed to add stdin to epoll\n");
        goto err;
    }

    event.events = EPOLLOUT | EPOLLET;
    event.data.u64 = 0;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, outfd, &event) < 0) {
        throw_err("Failed to add stdout to epoll\n");
        goto err;
    }

    /* Setup registry and buffers */
    fifo_clear(&priv->tx_buf);
    fifo_clear(&priv->rx_buf);

    /* Setup mutex */
    pthread_mutex_init(&priv->lock, NULL);
    pthread_mutex_init(&priv->loopback_lock, NULL);

    /* Create the thread*/
    priv->stop = false;
    if (pthread_create(&priv->worker, NULL, (void *) serial_thread,
                       (void *) s) < 0) {
        throw_err("Failed to create worker thread\n");
        goto err;
    }

    dev_init(&s->dev, COM1_PORT_BASE, COM1_PORT_SIZE, s, serial_handle_io);
    bus_register_dev(bus, &s->dev);

    priv->initialized = true;

    return 0;

err:
    close(infd);
    close(outfd);
    close(evfd);
    close(epollfd);

    return -1;
}

void serial_exit(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    __atomic_store_n(&priv->stop, true, __ATOMIC_RELAXED);
    pthread_join(priv->worker, NULL);

    /* Check loopback */
    if (priv->mcr & UART_MCR_LOOP)
        pthread_mutex_unlock(&priv->loopback_lock);

    close(priv->evfd);
    close(priv->infd);
    close(priv->outfd);
    close(priv->epollfd);

    priv->initialized = false;
}
