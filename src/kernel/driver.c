#include <xbook/driver.h>
#include <math.h>
#include <stdio.h>

#include <xbook/debug.h>
#include <assert.h>
#include <xbook/mdl.h>
#include <xbook/clock.h>
#include <string.h>
#include <xbook/memspace.h>
#include <sys/ioctl.h>
#include <xbook/config.h>
#include <xbook/virmem.h>
#include <xbook/schedule.h>
#include <xbook/initcall.h>
#include <xbook/fsal.h>

// #define DRIVER_FRAMEWROK_DEBUG

/* TODO:添加设备别名机制，通过别名来访问设备 */

LIST_HEAD(driver_list_head);
device_object_t *device_handle_table[DEVICE_HANDLE_NR];
DEFINE_SPIN_LOCK_UNLOCKED(driver_lock);

static iostatus_t default_device_dispatch(device_object_t *device, io_request_t *ioreq)
{
    ioreq->io_status.infomation = 0;
    ioreq->io_status.status = IO_SUCCESS;
    io_complete_request(ioreq);
    return IO_SUCCESS;
}

void drivers_print()
{
    driver_object_t *drvobj;
    device_object_t *devobj;
    int device_count;
    keprint(PRINT_INFO "io system info-> drivers\n");
    list_for_each_owner (drvobj, &driver_list_head, list) {
        keprint(PRINT_INFO "driver: name=%s\n", drvobj->name.text);
        device_count = 0;
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            keprint(PRINT_INFO "        device: name=%s\n", devobj->name.text);
            device_count++;
        }
        keprint(PRINT_INFO "        device: count=%d\n", device_count);
    }
}

void drivers_print_mini()
{
    driver_object_t *drvobj;
    device_object_t *devobj;
    keprint(PRINT_INFO "io system info-> drivers\n");
    list_for_each_owner (drvobj, &driver_list_head, list) {
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            keprint("%s ", devobj->name.text);
        }
    }
    keprint("\n");
}

static driver_object_t *io_search_driver_by_name(char *drvname)
{
    driver_object_t *drvobj;
    spin_lock(&driver_lock);
    list_for_each_owner (drvobj, &driver_list_head, list) {
        if (!strcmp(drvobj->name.text, drvname)) {
            spin_unlock(&driver_lock);
            return drvobj;
        }
    }
    spin_unlock(&driver_lock);
    return NULL;
}

device_object_t *device_handle_table_search_by_name(char *name)
{
    device_object_t *devobj;
    int i;
    for (i = 0; i < DEVICE_HANDLE_NR; i++) {
        devobj = device_handle_table[i];
        if (devobj != NULL) {
            if (!strcmp(devobj->name.text, name)) {
                return devobj;
            }
        }
    }
    return NULL;
}

handle_t device_handle_find_by_object(device_object_t *devobj)
{
    device_object_t *_devobj;
    int i;
    for (i = 0; i < DEVICE_HANDLE_NR; i++) {
        _devobj = device_handle_table[i];
        if (_devobj == devobj) {
            return i;
        }
    }
    return -1;
}

int device_handle_table_insert(device_object_t *devobj)
{
    device_object_t **_devobj;
    int i;
    for (i = 0; i < DEVICE_HANDLE_NR; i++) {
        _devobj = &device_handle_table[i];
        if (*_devobj == NULL) {
            *_devobj = devobj;
            return i;
        }
    }
    return -1;
}

int device_handle_table_remove(device_object_t *devobj)
{
    device_object_t **_devobj;
    int i;
    for (i = 0; i < DEVICE_HANDLE_NR; i++) {
        _devobj = &device_handle_table[i];
        if (*_devobj) {
            if (!strcmp((*_devobj)->name.text, devobj->name.text) &&
                (*_devobj)->type == devobj->type) {
                *_devobj = NULL;
                return 0;
            }
        }
    }
    return -1;
}

device_object_t *io_search_device_by_name(char *name)
{
    driver_object_t *drvobj;
    device_object_t *devobj;      
    spin_lock(&driver_lock);
    devobj = device_handle_table_search_by_name(name);
    if (devobj) {
        spin_unlock(&driver_lock);
        return devobj;
    }
    list_for_each_owner (drvobj, &driver_list_head, list) {
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            if (!strcmp(devobj->name.text, name)) {
                spin_unlock(&driver_lock);
                return devobj;
            }
        }
    }
    spin_unlock(&driver_lock);
    return NULL;
}

void driver_object_init(driver_object_t *driver)
{
    list_init(&driver->device_list);
    list_init(&driver->list);
    driver->drver_extension = NULL;
    driver->driver_enter = NULL;
    driver->driver_exit = NULL;
    int i;
    for (i = 0; i < MAX_IOREQ_FUNCTION_NR; i++) {
        driver->dispatch_function[i] = default_device_dispatch;
    }
    string_init(&driver->name);
    spinlock_init(&driver->device_lock);
}

int driver_object_create(driver_func_t func)
{
    driver_object_t *drvobj;
    iostatus_t status;
    drvobj = mem_alloc(sizeof(driver_object_t));
    if (drvobj == NULL)
        return -1;
    driver_object_init(drvobj);
    status = func(drvobj);
    if (status != IO_SUCCESS) {
        mem_free(drvobj);
        return -1;
    }
    if (drvobj->driver_enter)
        status = drvobj->driver_enter(drvobj); 

    if (status != IO_SUCCESS) {
        mem_free(drvobj);
        return -1;
    }

    unsigned long flags;        
    spin_lock_irqsave(&driver_lock, flags);
    assert(!list_find(&drvobj->list, &driver_list_head));
    list_add_tail(&drvobj->list, &driver_list_head);
    spin_unlock_irqrestore(&driver_lock, flags);

    return 0;
}

int driver_object_delete(driver_object_t *driver)
{
    iostatus_t status = IO_SUCCESS;
    if (driver->driver_exit)
        status = driver->driver_exit(driver); 
    if (status != IO_SUCCESS) {
        return -1;
    }
    unsigned long flags;        
    spin_lock_irqsave(&driver_lock, flags);
    assert(list_find(&driver->list, &driver_list_head));
    list_del(&driver->list);
    spin_unlock_irqrestore(&driver_lock, flags);
    mem_free(driver);
#ifdef DRIVER_FRAMEWROK_DEBUG
    keprint(PRINT_DEBUG "driver_object_delete: driver delete done.\n");
#endif

    return status;
}

void io_device_queue_init(device_queue_t *queue)
{
    spinlock_init(&queue->lock);
    list_init(&queue->list_head);
    wait_queue_init(&queue->wait_queue);
    queue->entry_count = 0;
}

device_object_t *io_iterative_search_device_by_type(device_object_t *devptr, device_type_t type)
{
    driver_object_t *drvobj;
    device_object_t *devobj;
    int flags = 0;
    spin_lock(&driver_lock);
    list_for_each_owner (drvobj, &driver_list_head, list) {
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            if (devobj->type == type) {   
                if (devptr == NULL) {
                    spin_unlock(&driver_lock);
                    return devobj;
                } else {
                    if (flags) {
                        spin_unlock(&driver_lock);
                        return devobj;
                    }
                    if (devptr == devobj) {
                        flags = 1;
                    }
                }
            }
        }
    }
    spin_unlock(&driver_lock);
    return NULL;
}


/**
 * sys_scandev - 扫描某种类型的设备
 * @de: 输入的设备项
 * @type: 设备类型
 * @out: 输出设备项
 * @return: 成功返回0，失败返回-1
 */
int sys_scandev(devent_t *de, device_type_t type, devent_t *out)
{
    if (!out)
        return -1;
    driver_object_t *drvobj;
    device_object_t *devobj;
    int flags = 0;
    spin_lock(&driver_lock);
    list_for_each_owner (drvobj, &driver_list_head, list) {
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            if (devobj->type == type) {
                if (de == NULL) {
                    memset(out->de_name, 0, DEVICE_NAME_LEN);
                    strcpy(out->de_name, devobj->name.text);
                    out->de_type = type;
                    spin_unlock(&driver_lock);
                    return 0;
                } else {
                    if (flags) {
                        memset(out->de_name, 0, DEVICE_NAME_LEN);
                        strcpy(out->de_name, devobj->name.text);
                        out->de_type = type;
                        spin_unlock(&driver_lock);
                        return 0;
                    }
                    if (!strcmp(de->de_name, devobj->name.text)) {
                        flags = 1;
                    }
                }
            }
        }
    }
    spin_unlock(&driver_lock);
    return -1;
}

/**
 * 注意：device参数是一个需要传回的设备指针
 * @return: 成功返回IO_SUCCESS，失败返回IO_FAILED
 */
iostatus_t io_create_device(
    driver_object_t *driver,
    unsigned long device_extension_size,
    char *device_name,
    device_type_t type,
    device_object_t **device
) {
    device_object_t *devobj = mem_alloc(sizeof(device_object_t) + device_extension_size);
    if (devobj == NULL)
        return IO_FAILED;
    list_init(&devobj->list);
    devobj->type = type;
    if (device_extension_size > 0)
        devobj->device_extension = (void *) (devobj + 1); /* 设备扩展的空间位于设备末尾 */
    else /* 没有扩展就指向NULL */
        devobj->device_extension = NULL;
    devobj->flags = 0;
    atomic_set(&devobj->reference, 0);
    devobj->cur_ioreq = NULL;
    devobj->reserved = 0;
    if (string_new(&devobj->name, device_name, DEVICE_NAME_LEN)) {
        mem_free(devobj);
        return IO_FAILED;
    }
    devobj->driver = driver;
    spinlock_init(&devobj->lock.spinlock);    /* 初始化设备锁-自旋锁 */
    mutexlock_init(&devobj->lock.mutexlock);  /* 初始化设备锁-互斥锁 */
    spin_lock(&driver->device_lock);
    assert(!list_find(&devobj->list, &driver->device_list));
    list_add_tail(&devobj->list, &driver->device_list);
    spin_unlock(&driver->device_lock);
    *device = devobj;
#ifdef DRIVER_FRAMEWROK_DEBUG
    keprint(PRINT_DEBUG "io_create_device: create device done.\n");
#endif
    return IO_SUCCESS;
}

void io_delete_device(
    device_object_t *device
) {
    if (device == NULL)
        return;
    spin_lock(&driver_lock);
    device_object_t *devobj = device_handle_table_search_by_name(device->name.text);
    if (devobj) {
        keprint(PRINT_NOTICE "io_delete_device: device %s is using!\n", 
            devobj->name.text);
        device_handle_table_remove(devobj);
    }
    spin_unlock(&driver_lock);
    devobj = device;
    driver_object_t *driver = device->driver;
    spin_lock(&driver->device_lock);
    assert(list_find(&devobj->list, &driver->device_list));
    list_del(&devobj->list);
    spin_unlock(&driver->device_lock);
    string_del(&devobj->name);
    mem_free(devobj);
}

io_request_t *io_request_alloc()
{
    io_request_t *ioreq = mem_alloc(sizeof(io_request_t));
    if (ioreq)
        memset(ioreq, 0, sizeof(io_request_t));
    return ioreq;
}

void io_request_free(io_request_t *ioreq)
{
    mem_free(ioreq);    
}

iostatus_t io_call_dirver(device_object_t *device, io_request_t *ioreq)
{
    iostatus_t status= IO_SUCCESS;

    driver_dispatch_t func = NULL;

    /* 根据设备类型选择不同的锁 */
    switch (device->type)
    {
    case DEVICE_TYPE_SERIAL_PORT:
    case DEVICE_TYPE_SCREEN:
    case DEVICE_TYPE_KEYBOARD:
    case DEVICE_TYPE_MOUSE:
    case DEVICE_TYPE_VIRTUAL_CHAR:
    case DEVICE_TYPE_BEEP:
    case DEVICE_TYPE_VIEW:
        spin_lock(&device->lock.spinlock);
        break;
    case DEVICE_TYPE_DISK:
    case DEVICE_TYPE_NETWORK:
    case DEVICE_TYPE_PHYSIC_NETCARD:
        mutex_lock(&device->lock.mutexlock);
        break;
    default:
        break;
    }

    if (ioreq->flags & IOREQ_OPEN_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_OPEN];
    } else if (ioreq->flags & IOREQ_CLOSE_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_CLOSE];
    } else if (ioreq->flags & IOREQ_READ_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_READ];
    } else if (ioreq->flags & IOREQ_WRITE_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_WRITE];
    } else if (ioreq->flags & IOREQ_DEVCTL_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_DEVCTL];
    } else if (ioreq->flags & IOREQ_MMAP_OPERATION) {
        func = device->driver->dispatch_function[IOREQ_MMAP];
    }
    if (func)
        status = func(device, ioreq);
    
    return status;
}


static iostatus_t fastio_call_dirver(device_object_t *device, int arg, void *buf, int dispatch)
{
    iostatus_t status= IO_SUCCESS;
    driver_dispatch_fastio_t func = NULL;
    /* 根据设备类型选择不同的锁 */
    switch (device->type) {
    case DEVICE_TYPE_VIEW:
        spin_lock(&device->lock.spinlock);
        break;
    default:
        break;
    }
    func = (driver_dispatch_fastio_t )device->driver->dispatch_function[dispatch];
    if (func)
        status = func(device, arg, buf);

    switch (device->type) {
    case DEVICE_TYPE_VIEW:
        spin_unlock(&device->lock.spinlock);
        break;
    default:
        break;
    }
    return status;
}

io_request_t *io_build_sync_request(
    unsigned long function,
    device_object_t *devobj,
    void *buffer,
    unsigned long length,
    unsigned long offset,
    io_status_block_t *io_status_block
){
    io_request_t *ioreq = io_request_alloc();
    if (ioreq == NULL) {
        io_status_block->status = IO_FAILED;
        return NULL;
    }
    ioreq->devobj = devobj;
    if (io_status_block) {
        ioreq->io_status = *io_status_block;
    } else {
        ioreq->io_status.infomation = 0;
        ioreq->io_status.status = IO_FAILED;
    }
    list_init(&ioreq->list);
    ioreq->system_buffer = NULL;
    ioreq->user_buffer = buffer;
    ioreq->mdl_address = NULL;
    if (buffer) {    
        if (devobj->flags & DO_BUFFERED_IO) {
            if (length >= MAX_MEM_CACHE_SIZE) {
                length = MAX_MEM_CACHE_SIZE;
                keprint(PRINT_WARING "io_build_sync_request: length too big!\n");
            }
            ioreq->system_buffer = mem_alloc(length);
            if (ioreq->system_buffer == NULL) {
                mem_free(ioreq);
                return NULL;
            }
            ioreq->flags |= IOREQ_BUFFERED_IO;
        } else if (devobj->flags & DO_DIRECT_IO) {
            ioreq->mdl_address = mdl_alloc(buffer, length, FALSE, ioreq);
            if (ioreq->mdl_address == NULL) {
                mem_free(ioreq);
                return NULL;    
            }
            /* 分配内存描述列表 */
        } /* 直接使用用户地址 */
    }
    unsigned long flags;

    switch (function)
    {
    case IOREQ_OPEN:
        ioreq->flags |= IOREQ_OPEN_OPERATION;
        ioreq->parame.open.devname = NULL;
        ioreq->parame.open.flags = 0;
        break;
    case IOREQ_CLOSE:
        ioreq->flags |= IOREQ_CLOSE_OPERATION;
        break;
    case IOREQ_READ:
        ioreq->flags |= IOREQ_READ_OPERATION;
        ioreq->parame.read.length = length;
        ioreq->parame.read.offset = offset;
        break;
    case IOREQ_WRITE:
        ioreq->flags |= IOREQ_WRITE_OPERATION;
        ioreq->parame.write.length = length;
        ioreq->parame.write.offset = offset;
        if (devobj->flags & DO_BUFFERED_IO) {
            interrupt_save_and_disable(flags);
            memcpy(ioreq->system_buffer, buffer, length);
            interrupt_restore_state(flags);
        }
        break;
    case IOREQ_DEVCTL:
        ioreq->flags |= IOREQ_DEVCTL_OPERATION;
        ioreq->parame.devctl.code = 0;
        ioreq->parame.devctl.arg = 0;
        break;
    case IOREQ_MMAP:
        ioreq->flags |= IOREQ_MMAP_OPERATION;
        ioreq->parame.mmap.flags = offset;
        ioreq->parame.mmap.length = length;
        break;
    default:
        break;
    }
    return ioreq;
}

void io_complete_request(io_request_t *ioreq)
{
    if (ioreq->io_status.status == IO_FAILED)
        ioreq->io_status.infomation = -1;
    
    ioreq->flags |= IOREQ_COMPLETION;
    /* 根据设备类型选择不同的锁 */
    switch (ioreq->devobj->type)
    {
    case DEVICE_TYPE_SERIAL_PORT:
    case DEVICE_TYPE_SCREEN:
    case DEVICE_TYPE_KEYBOARD:
    case DEVICE_TYPE_MOUSE:
    case DEVICE_TYPE_VIRTUAL_CHAR:
    case DEVICE_TYPE_BEEP:
    case DEVICE_TYPE_VIEW:
        spin_unlock(&ioreq->devobj->lock.spinlock);
        break;
    case DEVICE_TYPE_DISK:
    case DEVICE_TYPE_NETWORK:
    case DEVICE_TYPE_PHYSIC_NETCARD:
        mutex_unlock(&ioreq->devobj->lock.mutexlock);
        break;
    default:
        break;
    }
}

static int io_complete_check(io_request_t *ioreq, iostatus_t status)
{
    if (status == IO_SUCCESS) {
        if (ioreq->io_status.status == IO_SUCCESS && 
            ioreq->flags & IOREQ_COMPLETION) {
            return 0;
        }
    }
    return -1;
}

void io_device_queue_cleanup(device_queue_t *queue)
{
    device_queue_entry_t *entry, *next;
    unsigned long irqflags;
    spin_lock_irqsave(&queue->lock, irqflags);
    list_for_each_owner_safe (entry, next, &queue->list_head, list) {
        list_del(&entry->list);
        mem_free(entry);
    }
    spin_unlock_irqrestore(&queue->lock, irqflags);
}

iostatus_t io_device_queue_append(device_queue_t *queue, unsigned char *buf, int len)
{
    unsigned long irqflags;
    spin_lock_irqsave(&queue->lock, irqflags);
    if (queue->entry_count > DEVICE_QUEUE_ENTRY_NR) { /* 超过队列项数，就先丢弃数据包 */
        spin_unlock_irqrestore(&queue->lock, irqflags);
        return IO_FAILED;
    }
    device_queue_entry_t *entry = mem_alloc(sizeof(device_queue_entry_t) + len);
    if (entry == NULL) {
        spin_unlock_irqrestore(&queue->lock, irqflags);
        return IO_FAILED;
    }
    list_add_tail(&entry->list, &queue->list_head);
    queue->entry_count++;
    entry->buf = (unsigned char *) (entry + 1);
    entry->length = len;
    memcpy(entry->buf, buf, len);
    spin_unlock_irqrestore(&queue->lock, irqflags);
    wait_queue_wakeup(&queue->wait_queue);
    return IO_SUCCESS;
}

int io_device_queue_pickup(device_queue_t *queue, unsigned char *buf, int buflen, int flags)
{
    unsigned long irqflags;
    spin_lock_irqsave(&queue->lock, irqflags);
    if (!queue->entry_count) {  /* 没有数据包 */
        if (flags & IO_NOWAIT) {    /* 不进行等待 */
            spin_unlock_irqrestore(&queue->lock, irqflags);
            return -1;    
        }
        wait_queue_add(&queue->wait_queue, task_current);
        spin_unlock_irqrestore(&queue->lock, irqflags);
        task_block(TASK_BLOCKED);
        spin_lock_irqsave(&queue->lock, irqflags);   
    }
    device_queue_entry_t *entry;
    entry = list_first_owner(&queue->list_head, device_queue_entry_t, list);
    list_del(&entry->list);
    queue->entry_count--;
    int len = MIN(entry->length, buflen);
    memcpy(buf, entry->buf, len);
    mem_free(entry);
    spin_unlock_irqrestore(&queue->lock, irqflags);
#if DEBUG_LOCLA == 1
    keprint(PRINT_DEBUG "io_device_queue_get: pid=%d len=%d.\n",
        queue->wait_queue.task->pid, len);
#endif            
    return len;
}

iostatus_t io_device_increase_reference(device_object_t *devobj)
{
    if (atomic_get(&devobj->reference) >= 0) {
        atomic_inc(&devobj->reference);
    } else {
        keprint(PRINT_ERR "device_open: reference %d error!\n", atomic_get(&devobj->reference));
        return IO_FAILED;
    }
    return IO_SUCCESS;
}

iostatus_t io_device_decrease_reference(device_object_t *devobj)
{
    if (atomic_get(&devobj->reference) >= 0) {
        atomic_dec(&devobj->reference);
    } else {
        keprint(PRINT_ERR "device_close: reference %d error!\n", atomic_get(&devobj->reference));
        return IO_FAILED;    
    }
    return IO_SUCCESS;
}

handle_t device_open(char *devname, unsigned int flags)
{
    device_object_t *devobj = io_search_device_by_name(devname);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_open: device %s not found!\n", devname);
        return -1;
    }
    iostatus_t status = io_device_increase_reference(devobj);
    if (status == IO_FAILED) {
        keprint(PRINT_ERR "device_open: increase reference failed!\n");
        return -1;
    }
    io_request_t *ioreq = NULL;
    if (atomic_get(&devobj->reference) == 1) {
        ioreq = io_build_sync_request(IOREQ_OPEN, devobj, NULL, 0, 0, NULL);
        if (ioreq == NULL) {
            keprint(PRINT_ERR "device_open: alloc io request packet failed!\n", atomic_get(&devobj->reference));
            goto rollback_ref;
        }
        ioreq->parame.open.devname = devname;
        ioreq->parame.open.flags = flags;
        status = io_call_dirver(devobj, ioreq);
        if (!io_complete_check(ioreq, status)) {
            io_request_free((ioreq));
            spin_lock(&driver_lock);
            handle_t handle = device_handle_table_insert(devobj);
            if (handle == -1) {
                keprint(PRINT_ERR "device_open: insert device handle tabel failed!\n");
                spin_unlock(&driver_lock);
                return -1;    
            }
            spin_unlock(&driver_lock);
            return handle;
        }
        io_request_free(ioreq);
        goto rollback_ref;
    } else {
        handle_t handle = device_handle_find_by_object(devobj);
        return handle;
    }
rollback_ref:
    keprint(PRINT_ERR "device_open: do dispatch failed!\n");
    io_device_decrease_reference(devobj);
    return -1;
}

int device_close(handle_t handle)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_close: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }
    
    iostatus_t status = io_device_decrease_reference(devobj);
    if (status == IO_FAILED) {
        return -1;
    }
    io_request_t *ioreq = NULL;
    if (!atomic_get(&devobj->reference)) { /* 最后一次关闭才关闭 */    
        ioreq = io_build_sync_request(IOREQ_CLOSE, devobj, NULL, 0, 0, NULL);
        if (ioreq == NULL) {
            keprint(PRINT_ERR "device_close: alloc io request packet failed!\n", atomic_get(&devobj->reference));
            goto rollback_ref;
        }
        status = io_call_dirver(devobj, ioreq);
        if (!io_complete_check(ioreq, status)) {
            spin_lock(&driver_lock);
            if (device_handle_table_remove(devobj)) {
                keprint(PRINT_ERR "device_close: device=%s remove from device handle table failed!\n",
                    devobj->name.text);
                spin_unlock(&driver_lock);
                return -1;
            }
            spin_unlock(&driver_lock);
            io_request_free((ioreq));
            return 0;
        }
        io_request_free(ioreq);
        goto rollback_ref;
    } else {
        return 0;
    }
rollback_ref:
    io_device_increase_reference(devobj);
    return -1;
}

void *device_mmap(handle_t handle, size_t length, int flags)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return NULL;

    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "%s: device object error by handle=%d!\n", __func__, handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return NULL;
    }
    
    iostatus_t status = IO_SUCCESS;
    io_request_t *ioreq = io_build_sync_request(IOREQ_MMAP, devobj, NULL, length, flags, NULL);
    if (ioreq == NULL) {
        keprint(PRINT_ERR "%s: alloc io request packet failed!\n", __func__);
        return NULL;
    }

    status = io_call_dirver(devobj, ioreq);
    if (!io_complete_check(ioreq, status)) {
        void *mapaddr = NULL;
        if (ioreq->io_status.infomation) {
            // dbgprint("device memmap paddr=%x, len=%x\n", ioreq->io_status.infomation, length);

            if (flags & IO_KERNEL)
                mapaddr = memio_remap(ioreq->io_status.infomation, length);
            else
                mapaddr = mem_space_mmap(0, ioreq->io_status.infomation, length, 
                    PROT_USER | PROT_WRITE, MEM_SPACE_MAP_SHARED | MEM_SPACE_MAP_REMAP);
        }
        io_request_free((ioreq));
        return mapaddr;
    }
    io_request_free((ioreq));
    return NULL;
}

int device_incref(handle_t handle)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_close: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }
    if (io_device_increase_reference(devobj) == IO_SUCCESS)
        return 0;
    return -1;
}

int device_decref(handle_t handle)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_close: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }
    if (io_device_decrease_reference(devobj) == IO_SUCCESS)
        return 0;
    return -1;
}

ssize_t device_read(handle_t handle, void *buffer, size_t length, off_t offset)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_read: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }
    int len;
    iostatus_t status = IO_SUCCESS;
    io_request_t *ioreq = NULL;
    ioreq = io_build_sync_request(IOREQ_READ, devobj, buffer, length, offset, NULL);
    if (ioreq == NULL) {
        keprint(PRINT_ERR "device_read: alloc io request packet failed!\n");
        return -1;
    }

    status = io_call_dirver(devobj, ioreq);
    if (!io_complete_check(ioreq, status)) {
        len = ioreq->io_status.infomation;
        if (devobj->flags & DO_BUFFERED_IO) { 
            unsigned long flags;
            interrupt_save_and_disable(flags);
            memcpy(ioreq->user_buffer, ioreq->system_buffer, len);
            interrupt_restore_state(flags);
            mem_free(ioreq->system_buffer);
        } else if (devobj->flags & DO_DIRECT_IO) { 
            keprint(PRINT_DEBUG "device_read: read done. free mdl.\n");
            mdl_free(ioreq->mdl_address);
            ioreq->mdl_address = NULL;
        }
        io_request_free((ioreq));
        return len;
    }
#ifdef DRIVER_FRAMEWROK_DEBUG
    keprint(PRINT_ERR "device_read: do dispatch failed!\n");
#endif
/* rollback_ioreq */
    io_request_free(ioreq);
    return -1;
}

ssize_t device_write(handle_t handle, void *buffer, size_t length, off_t offset)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_write: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }

    iostatus_t status = IO_SUCCESS;
    io_request_t *ioreq = NULL;
    ioreq = io_build_sync_request(IOREQ_WRITE, devobj, buffer, length, offset, NULL);
    if (ioreq == NULL) {
        keprint(PRINT_ERR "device_write: alloc io request packet failed!\n");
        return -1;
    }
    status = io_call_dirver(devobj, ioreq);

    if (!io_complete_check(ioreq, status)) {
        if (devobj->flags & DO_DIRECT_IO) { 
            keprint(PRINT_DEBUG "device_write: write done. free mdl.\n");
            mdl_free(ioreq->mdl_address);
            ioreq->mdl_address = NULL;
        }
        unsigned int len = ioreq->io_status.infomation;
        io_request_free((ioreq));
        return len;
    }
#ifdef DRIVER_FRAMEWROK_DEBUG
    keprint(PRINT_ERR "device_write: do dispatch failed!\n");
#endif
/* rollback_ioreq */
    io_request_free(ioreq);
    return -1;
}

ssize_t device_devctl(handle_t handle, unsigned int code, unsigned long arg)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_devctl: device object error by handle=%d!\n", handle);
        /* 应该激活一个触发器，让调用者停止运行 */
        return -1;
    }

    iostatus_t status = IO_SUCCESS;
    io_request_t *ioreq = NULL;
    ioreq = io_build_sync_request(IOREQ_DEVCTL, devobj, NULL, 0, 0, NULL);
    if (ioreq == NULL) {
        keprint(PRINT_ERR "device_devctl: alloc io request packet failed!\n");
        return -1;
    }
    ioreq->parame.devctl.code = code;
    ioreq->parame.devctl.arg = arg;
    
    status = io_call_dirver(devobj, ioreq);
    if (!io_complete_check(ioreq, status)) {
        unsigned int infomation = ioreq->io_status.infomation;
        io_request_free((ioreq));
        return infomation;
    }
#ifdef DRIVER_FRAMEWROK_DEBUG
    keprint(PRINT_ERR "device_devctl: do dispatch failed!\n");
#endif
/* rollback_ioreq */
    io_request_free(ioreq);
    return -1;
}

int io_uninstall_driver(char *drvname)
{
    driver_object_t *drvobj;
    drvobj = io_search_driver_by_name(drvname);
    if (!drvobj)
        return -1;
    if (driver_object_delete(drvobj)) {
        keprint(PRINT_ERR "io_uninstall_driver: delete driver %s failed!\n", drvname);
    }
    return 0;
}

void dump_device_object(device_object_t *device)
{
    keprint(PRINT_DEBUG "dump_device_object: type=%d driver=%x extension=%x flags=%x reference=%x name=%s\n",
        device->type, device->driver, device->device_extension, device->flags,
        atomic_get(&device->reference), device->name.text);
}

int device_probe_unused(const char *name, char *buf, size_t buflen)
{
    driver_object_t *drvobj;
    device_object_t *devobj;
    int namelen = strlen(name);
    list_for_each_owner (drvobj, &driver_list_head, list) {
        list_for_each_owner (devobj, &drvobj->device_list, list) {
            if (!strncmp(name, devobj->name.text, namelen)) {
                if (!atomic_get(&devobj->reference)) {
                    memcpy(buf, devobj->name.text, min(buflen, DEVICE_NAME_LEN));
                    buf[buflen - 1] = '\0';
                    return 0;
                }
            }
        }
    }
    return -1;
}


int input_even_init(input_even_buf_t *evbuf)
{
    spinlock_init(&evbuf->lock);
    evbuf->head = evbuf->tail = 0;
    memset(evbuf->evbuf, 0, sizeof(input_event_t) * EVBUF_SIZE);
    return 0;
}


int input_even_put(input_even_buf_t *evbuf, input_event_t *even)
{
    unsigned long flags;
    spin_lock_irqsave(&evbuf->lock, flags);
    evbuf->evbuf[evbuf->head++] = *even;
    evbuf->head &= EVBUF_SIZE - 1;
    spin_unlock_irqrestore(&evbuf->lock, flags);
    return 0;
}

int input_even_get(input_even_buf_t *evbuf, input_event_t *even)
{
    unsigned long flags;
    spin_lock_irqsave(&evbuf->lock, flags);
    if (evbuf->head == evbuf->tail) {
        spin_unlock_irqrestore(&evbuf->lock, flags);
        return -1;    
    }
    *even = evbuf->evbuf[evbuf->tail++];
    evbuf->tail &= EVBUF_SIZE - 1;
    spin_unlock_irqrestore(&evbuf->lock, flags);
    return 0;
}

static int devif_open(void *pathname, int flags)
{
    char *p = (char *) pathname;
    return device_open(p, flags);
}

static int devif_close(int handle)
{
    return device_close(handle);
}

static int devif_incref(int handle)
{
    return device_incref(handle);
}

static int devif_decref(int handle)
{
    return device_decref(handle);
}

static int devif_read(int handle, void *buf, size_t size)
{
    return device_read(handle, buf, size, DISKOFF_MAX);
}

static int devif_write(int handle, void *buf, size_t size)
{
    return device_write(handle, buf, size, DISKOFF_MAX);
}

static int devif_ioctl(int handle, int cmd, unsigned long arg)
{
    return device_devctl(handle, cmd, arg);
}

static int devif_lseek(int handle, off_t off, int whence)
{
    return device_devctl(handle, DISKIO_SETOFF, (unsigned long) &off);
}

static size_t devif_fsize(int handle)
{
    int size = 0;
    if (device_devctl(handle, DISKIO_GETSIZE, (unsigned long) &size) < 0)
        return 0;
    return size;
}

static off_t devif_ftell(int handle)
{
    off_t off = 0;
    if (device_devctl(handle, DISKIO_GETOFF, (unsigned long) &off) < 0)
        return 0;
    return off;
}

static void *devif_mmap(int handle, size_t length, int flags)
{
    return device_mmap(handle, length, flags);
}

static int devif_fastio(int handle, int cmd, void *arg)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_read: device object error by handle=%d!\n", handle);
        return -1;
    }
    iostatus_t status = IO_SUCCESS;
    status = fastio_call_dirver(devobj, cmd, arg, IOREQ_FASTIO);
    if (status == IO_SUCCESS)
        return 0;
    return -1;
}

static int devif_fastread(int handle, void *buf, size_t size)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_read: device object error by handle=%d!\n", handle);
        return -1;
    }
    iostatus_t status = IO_SUCCESS;
    status = fastio_call_dirver(devobj, size, buf, IOREQ_FASTREAD);
    if (status == IO_SUCCESS)
        return 0;
    return -1;
}

static int devif_fastwrite(int handle, void *buf, size_t size)
{
    if (IS_BAD_DEVICE_HANDLE(handle))
        return -1;
    device_object_t *devobj = GET_DEVICE_BY_HANDLE(handle);
    if (devobj == NULL) {
        keprint(PRINT_ERR "device_read: device object error by handle=%d!\n", handle);
        return -1;
    }
    iostatus_t status = IO_SUCCESS;
    status = fastio_call_dirver(devobj, size, buf, IOREQ_FASTWRITE);
    if (status == IO_SUCCESS)
        return 0;
    return -1;
}

fsal_t devif;

void driver_framewrok_init()
{
    int i;
    for (i = 0; i < DEVICE_HANDLE_NR; i++) {
        device_handle_table[i] = NULL;
    }
    memset(&devif, 0, sizeof(fsal_t));
    devif.name = "devif";
    devif.open      = devif_open;
    devif.close     = devif_close;
    devif.incref    = devif_incref;
    devif.decref    = devif_decref;
    devif.read      = devif_read;
    devif.write     = devif_write;
    devif.ioctl     = devif_ioctl;
    devif.lseek     = devif_lseek;
    devif.fsize     = devif_fsize;
    devif.ftell     = devif_ftell;
    devif.mmap      = devif_mmap;
    devif.fastio    = devif_fastio;
    devif.fastread  = devif_fastread;
    devif.fastwrite = devif_fastwrite;
}
