#include <linux/serial_reg.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "err.h"
#include "serial.h"
#include "utils.h"
#include "vm.h"

#define SERIAL_IRQ 4
#define IO_READ8(data) *((uint8_t *) data)
#define IO_WRITE8(data, value) ((uint8_t *) data)[0] = value

struct serial_dev_priv {
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

    struct fifo rx_buf;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static struct serial_dev_priv serial_dev_priv = {
    .iir = UART_IIR_NO_INT,
    .mcr = UART_MCR_OUT2,
    .lsr = UART_LSR_TEMT | UART_LSR_THRE,
    .msr = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

/* FIXME: This implementation is incomplete */
static void serial_update_irq(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;
    uint8_t iir = UART_IIR_NO_INT;

    /* If enable receiver data interrupt and receiver data ready */
    if ((priv->ier & UART_IER_RDI) && (priv->lsr & UART_LSR_DR))
        iir = UART_IIR_RDI;
    /* If enable transmiter data interrupt and transmiter empty */
    else if ((priv->ier & UART_IER_THRI) && (priv->lsr & UART_LSR_TEMT))
        iir = UART_IIR_THRI;

    priv->iir = iir | 0xc0;

    /* FIXME: the return error of vm_irq_line should be handled */
    vm_irq_line(container_of(s, vm_t, serial), s->irq_num,
                iir == UART_IIR_NO_INT ? 0 /* inactive */ : 1 /* active */);
}

static int serial_readable(serial_dev_t *s, int timeout)
{
    struct pollfd pollfd = (struct pollfd){
        .fd = s->infd,
        .events = POLLIN,
    };
    return (poll(&pollfd, 1, timeout) > 0) && (pollfd.revents & POLLIN);
}

#define FREQ_NS ((int) (1.0e6))
#define NS_PER_SEC ((int) (1.0e9))

/* global state to stop the loop of thread */
static volatile bool thread_stop = false;

static void *serial_thread(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *)s->priv;
    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        if (serial_readable(s, -1)) {
            pthread_mutex_lock(&priv->lock);
            if (fifo_is_full(&priv->rx_buf))
                pthread_cond_wait(&priv->cond, &priv->lock);

            /* rx_buf is not full */
            serial_console(s);
            pthread_mutex_unlock(&priv->lock);
        }
    }

    return NULL;
}

void serial_console(serial_dev_t *s)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;

    if (priv->lsr & UART_LSR_DR || !fifo_is_empty(&priv->rx_buf))
        return;

    while (!fifo_is_full(&priv->rx_buf) && serial_readable(s, 0)) {
        char c;
        if (read(s->infd, &c, 1) == -1)
            break;
        if (!fifo_put(&priv->rx_buf, c))
            break;
        priv->lsr |= UART_LSR_DR;
    }
    serial_update_irq(s);
}

static void serial_in(serial_dev_t *s, uint16_t offset, void *data)
{
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s->priv;

    switch (offset) {
    case UART_RX:
        if (priv->lcr & UART_LCR_DLAB) {
            IO_WRITE8(data, priv->dll);
        } else {
            if (fifo_is_empty(&priv->rx_buf))
                break;

            uint8_t value;
            if (fifo_get(&priv->rx_buf, value))
                IO_WRITE8(data, value);

            if (fifo_is_empty(&priv->rx_buf)) {
                priv->lsr &= ~UART_LSR_DR;
                serial_update_irq(s);
            }

            if (fifo_level(&priv->rx_buf) < FIFO_LEN / 2)
                /* The buffer is half full, try to read more */
                pthread_cond_signal(&priv->cond);
        }
        break;
    case UART_IER:
        if (priv->lcr & UART_LCR_DLAB)
            IO_WRITE8(data, priv->dlm);
        else
            IO_WRITE8(data, priv->ier);
        break;
    case UART_IIR:
        IO_WRITE8(data, priv->iir | 0xc0); /* 0xc0 stands for FIFO enabled */
        break;
    case UART_LCR:
        IO_WRITE8(data, priv->lcr);
        break;
    case UART_MCR:
        IO_WRITE8(data, priv->mcr);
        break;
    case UART_LSR:
        IO_WRITE8(data, priv->lsr);
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

    switch (offset) {
    case UART_TX:
        if (priv->lcr & UART_LCR_DLAB) {
            priv->dll = IO_READ8(data);
        } else {
            priv->lsr |= (UART_LSR_TEMT | UART_LSR_THRE); /* flush TX */
            putchar(((char *) data)[0]);
            fflush(stdout);
            serial_update_irq(s);
        }
        break;
    case UART_IER:
        if (!(priv->lcr & UART_LCR_DLAB)) {
            priv->ier = IO_READ8(data);
            serial_update_irq(s);
        } else {
            priv->dlm = IO_READ8(data);
        }
        break;
    case UART_FCR:
        priv->fcr = IO_READ8(data);
        break;
    case UART_LCR:
        priv->lcr = IO_READ8(data);
        break;
    case UART_MCR:
        priv->mcr = IO_READ8(data);
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
    struct serial_dev_priv *priv = (struct serial_dev_priv *) s;
    void (*serial_op)(serial_dev_t *, uint16_t, void *) =
        is_write ? serial_out : serial_in;
    pthread_mutex_lock(&priv->lock);
    serial_op(s, offset, data);
    pthread_mutex_unlock(&priv->lock);
}

int serial_init(serial_dev_t *s, struct bus *bus)
{
    *s = (serial_dev_t){
        .priv = (void *) &serial_dev_priv,
        .main_tid = pthread_self(),
        .infd = STDIN_FILENO,
        .irq_num = SERIAL_IRQ
    };
    pthread_create(&s->worker_tid, NULL, (void *) serial_thread, (void *) s);

    dev_init(&s->dev, COM1_PORT_BASE, COM1_PORT_SIZE, s, serial_handle_io);
    bus_register_dev(bus, &s->dev);

    return 0;
}

void serial_exit(serial_dev_t *s)
{
    __atomic_store_n(&thread_stop, true, __ATOMIC_RELAXED);
    pthread_join(s->worker_tid, NULL);
}
