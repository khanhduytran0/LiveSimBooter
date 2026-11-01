// Kernel ABI constants are verified against XNU's mach_trap_table and SDK MIG message definitions.
// Compile without stack protectors and builtins: no instruction here may call into the detached host cache.
#define __MigPackStructs 1
#include "LocalMach.h"
#include "LocalRuntime.h"

#include <cstddef>
#include <cstdint>
#include <mach/mig_errors.h>
#include <mach/vm_map.h>

namespace lsb {
namespace {

constexpr uint64_t kSendKernelObject = UINT64_C(0x0000000200000000);
constexpr uint64_t kSendMessageQueue = UINT64_C(0x0000000400000000);
constexpr NDR_record_t kNDR = {0, 0, 0, 0, 1, 0, 0, 0};

// iOS omits mach_vm.h. Its 64-bit vm_map request layouts match mach_vm's wire fields, but the kernel IDs must be 4800-series.
// Sending these layouts to the legacy 3800-series vm32 subsystem produces MIG_TYPE_ERROR even in a 64-bit process.
static_assert(sizeof(vm_address_t) == sizeof(mach_vm_address_t) && sizeof(vm_size_t) == sizeof(mach_vm_size_t));
static_assert(sizeof(__Request__vm_allocate_t) == 52 && sizeof(__Reply__vm_allocate_t) == 44);
static_assert(sizeof(__Request__vm_deallocate_t) == 48 && sizeof(__Request__vm_protect_t) == 56 && sizeof(__Request__vm_copy_t) == 56);

uintptr_t addressOf(const void *value) { return reinterpret_cast<uintptr_t>(value); }
uint64_t pair(uint32_t low, uint32_t high) { return low | (uint64_t(high) << 32); }

mach_msg_return_t message(mach_msg_header_t *header, uint64_t options, mach_msg_size_t sendSize, mach_msg_size_t receiveSize,
                          mach_port_t receivePort, mach_msg_timeout_t timeout, mach_port_t priority)
{
    if (!header)
        return MACH_SEND_INVALID_DATA;
    uint32_t descriptors = 0;
    if ((options & MACH_SEND_MSG) && (header->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        if (sendSize < sizeof(mach_msg_base_t))
            return MACH_SEND_MSG_TOO_SMALL;
        descriptors = reinterpret_cast<mach_msg_base_t *>(header)->body.msgh_descriptor_count;
    }
    const uint64_t ports = pair(header->msgh_remote_port, header->msgh_local_port);
    const uint64_t voucherID = pair(header->msgh_voucher_port, header->msgh_id);
    const uint64_t descriptorPort = pair(descriptors, receivePort);
    const uint64_t receivePriority = pair(receiveSize, priority);
    uint64_t bitsSize = pair(header->msgh_bits, sendSize);
    uint64_t kernelOptions = options & ~(uint64_t(MACH_SEND_INTERRUPT) | MACH_RCV_INTERRUPT);
    for (;;) {
        auto result = static_cast<mach_msg_return_t>(machTrap(-47, addressOf(header), kernelOptions, bitsSize, ports,
                                                             voucherID, descriptorPort, receivePriority, timeout));
        if (result == MACH_SEND_INTERRUPTED && !(options & MACH_SEND_INTERRUPT))
            continue;
        if (result == MACH_RCV_INTERRUPTED && !(options & MACH_RCV_INTERRUPT)) {
            // The send already succeeded. Never resend a request after an interrupted receive.
            kernelOptions &= ~(uint64_t(MACH_SEND_MSG) | MACH_RCV_SYNC_WAIT);
            bitsSize &= UINT32_MAX;
            continue;
        }
        return result;
    }
}

template <typename Request, typename Reply> struct RPC {
    union {
        Request request;
        Reply reply;
        uint8_t storage[(sizeof(Request) > sizeof(Reply) ? sizeof(Request) : sizeof(Reply)) + sizeof(mach_msg_max_trailer_t)];
    } data;

    RPC() { lsb::zero(&data, sizeof(data)); }

    kern_return_t call(mach_port_t target, mach_msg_id_t id, bool complex = false)
    {
        const auto replyPort = static_cast<mach_port_t>(machTrap(-26));
        if (!MACH_PORT_VALID(replyPort))
            return KERN_RESOURCE_SHORTAGE;
        auto &header = data.request.Head;
        header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | (complex ? MACH_MSGH_BITS_COMPLEX : 0);
        header.msgh_size = sizeof(Request);
        header.msgh_remote_port = target;
        header.msgh_local_port = replyPort;
        header.msgh_id = id;
        auto result = message(&header, MACH_SEND_MSG | MACH_RCV_MSG | kSendKernelObject, sizeof(Request), sizeof(data), replyPort, 0, 0);
        (void)machTrap(-19, lsb::task_self_trap(), replyPort, MACH_PORT_RIGHT_RECEIVE, static_cast<uintptr_t>(-1));
        if (result != MACH_MSG_SUCCESS)
            return result;
        if (header.msgh_id != id + 100) {
            lsb::mach_msg_destroy(&header);
            return MIG_REPLY_MISMATCH;
        }
        if (header.msgh_size < sizeof(mach_msg_header_t) || header.msgh_size > sizeof(data))
            return MIG_TYPE_ERROR;
        if (header.msgh_bits & MACH_MSGH_BITS_COMPLEX)
            return KERN_SUCCESS;
        if (header.msgh_size < sizeof(mig_reply_error_t))
            return MIG_TYPE_ERROR;
        return reinterpret_cast<mig_reply_error_t *>(&header)->RetCode;
    }

    kern_return_t simple(mach_port_t target, mach_msg_id_t id, size_t minimumSize = sizeof(Reply), bool complexRequest = false)
    {
        auto result = call(target, id, complexRequest);
        if (result != KERN_SUCCESS)
            return result;
        if ((data.reply.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) || data.reply.Head.msgh_size < minimumSize) {
            lsb::mach_msg_destroy(&data.reply.Head);
            return MIG_TYPE_ERROR;
        }
        return KERN_SUCCESS;
    }
};

void outOfLine(mach_msg_ool_descriptor_t &descriptor, const void *data, size_t size)
{
    descriptor.address = const_cast<void *>(data);
    descriptor.size = static_cast<mach_msg_size_t>(size);
    descriptor.deallocate = false;
    descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
}

void destroyRight(mach_port_t port, mach_msg_type_name_t disposition)
{
    if (!MACH_PORT_VALID(port))
        return;
    auto task = lsb::task_self_trap();
    switch (disposition) {
        case MACH_MSG_TYPE_MOVE_RECEIVE:
            (void)machTrap(-19, task, port, MACH_PORT_RIGHT_RECEIVE, static_cast<uintptr_t>(-1));
            break;
        case MACH_MSG_TYPE_MOVE_SEND:
        case MACH_MSG_TYPE_MOVE_SEND_ONCE:
            (void)lsb::mach_port_deallocate(task, port);
            break;
        case MACH_MSG_TYPE_MAKE_SEND:
            if (lsb::mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS)
                (void)lsb::mach_port_deallocate(task, port);
            break;
        case MACH_MSG_TYPE_MAKE_SEND_ONCE: {
            RPC<__Request__mach_port_extract_right_t, __Reply__mach_port_extract_right_t> rpc;
            rpc.data.request.NDR = kNDR;
            rpc.data.request.name = port;
            rpc.data.request.msgt_name = MACH_MSG_TYPE_MAKE_SEND_ONCE;
            if (rpc.call(task, 3215) == KERN_SUCCESS && (rpc.data.reply.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
                rpc.data.reply.Head.msgh_size == sizeof(rpc.data.reply) && rpc.data.reply.msgh_body.msgh_descriptor_count == 1 &&
                rpc.data.reply.poly.type == MACH_MSG_PORT_DESCRIPTOR)
                (void)lsb::mach_port_deallocate(task, rpc.data.reply.poly.name);
            break;
        }
        default:
            break; // COPY_SEND and port names do not transfer ownership.
    }
}

template <typename Request> kern_return_t registerImages(task_t task, dyld_kernel_image_info_array_t images,
                                                        mach_msg_type_number_t count, mach_msg_id_t id)
{
    if (count > UINT32_MAX / sizeof(*images) || (count && !images))
        return KERN_INVALID_ARGUMENT;
    RPC<Request, mig_reply_error_t> rpc;
    rpc.data.request.msgh_body.msgh_descriptor_count = 1;
    outOfLine(rpc.data.request.dyld_images, images, count * sizeof(*images));
    rpc.data.request.NDR = kNDR;
    rpc.data.request.dyld_imagesCnt = count;
    return rpc.simple(task, id, sizeof(mig_reply_error_t), true);
}

} // namespace

kern_return_t vm_allocate(vm_map_t task, vm_address_t *address, vm_size_t size, int flags)
{
    auto result = static_cast<kern_return_t>(machTrap(-10, task, addressOf(address), size, flags));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    if (!address)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__vm_allocate_t, __Reply__vm_allocate_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.address = *address;
    rpc.data.request.size = size;
    rpc.data.request.flags = flags;
    result = rpc.simple(task, 4800);
    if (result == KERN_SUCCESS)
        *address = rpc.data.reply.address;
    return result;
}

kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size)
{
    auto result = static_cast<kern_return_t>(machTrap(-12, task, address, size));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    RPC<__Request__vm_deallocate_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.address = address;
    rpc.data.request.size = size;
    return rpc.simple(task, 4801);
}

kern_return_t vm_protect(vm_map_t task, vm_address_t address, vm_size_t size, boolean_t maximum, vm_prot_t protection)
{
    auto result = static_cast<kern_return_t>(machTrap(-14, task, address, size, maximum, protection));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    RPC<__Request__vm_protect_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.address = address;
    rpc.data.request.size = size;
    rpc.data.request.set_maximum = maximum;
    rpc.data.request.new_protection = protection;
    return rpc.simple(task, 4802);
}

// Diagnostic-local wrapper. mach_vm.defs exports this LP64 wire operation as ID 4815;
// SDK mach_vm.h also lists { "mach_vm_region_recurse", 4815 }. The legacy vm32 IDs are not interchangeable.
kern_return_t vm_region_recurse_64(vm_map_read_t task, vm_address_t *address, vm_size_t *size, natural_t *depth,
                                 vm_region_recurse_info_t info, mach_msg_type_number_t *count)
{
    using Request = __Request__vm_region_recurse_64_t;
    using Reply = __Reply__vm_region_recurse_64_t;
    static_assert(sizeof(Request) == 48 && offsetof(Reply, info) == 60 && sizeof(Reply) == 144);
    constexpr mach_msg_type_number_t capacity = sizeof(Reply::info) / sizeof(integer_t);
    if (!address || !size || !depth || !info || !count || !*count || *count > capacity) return KERN_INVALID_ARGUMENT;
    RPC<Request, Reply> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.address = *address;
    rpc.data.request.nesting_depth = *depth;
    rpc.data.request.infoCnt = *count;
    kern_return_t result = rpc.simple(task, 4815, offsetof(Reply, info));
    if (result != KERN_SUCCESS) return result;
    const Reply &reply = rpc.data.reply;
    if (reply.infoCnt > capacity || reply.infoCnt > *count ||
        reply.Head.msgh_size < offsetof(Reply, info) + size_t(reply.infoCnt) * sizeof(integer_t)) return MIG_TYPE_ERROR;
    for (mach_msg_type_number_t index = 0; index < reply.infoCnt; ++index) info[index] = reply.info[index];
    *address = reply.address;
    *size = reply.size;
    *depth = reply.nesting_depth;
    *count = reply.infoCnt;
    return KERN_SUCCESS;
}

kern_return_t vm_copy(vm_map_t task, vm_address_t source, vm_size_t size, vm_address_t destination)
{
    RPC<__Request__vm_copy_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.source_address = source;
    rpc.data.request.size = size;
    rpc.data.request.dest_address = destination;
    return rpc.simple(task, 4807);
}

mach_port_t mach_thread_self() { return static_cast<mach_port_t>(machTrap(-27)); }
mach_port_t task_self_trap() { return static_cast<mach_port_t>(machTrap(-28)); }
kern_return_t mach_timebase_info(mach_timebase_info_t info) { return static_cast<kern_return_t>(machTrap(-89, addressOf(info))); }
uint64_t mach_absolute_time() { return static_cast<uint64_t>(machTrap(-3)); }
kern_return_t thread_switch(mach_port_t thread, int option, mach_msg_timeout_t time)
{
    return static_cast<kern_return_t>(machTrap(-61, thread, option, time));
}

kern_return_t mach_port_deallocate(ipc_space_t task, mach_port_name_t name)
{
    auto result = static_cast<kern_return_t>(machTrap(-18, task, name));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    RPC<__Request__mach_port_deallocate_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.name = name;
    return rpc.simple(task, 3206);
}

kern_return_t mach_port_allocate(ipc_space_t task, mach_port_right_t right, mach_port_name_t *name)
{
    auto result = static_cast<kern_return_t>(machTrap(-16, task, right, addressOf(name)));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    if (!name)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__mach_port_allocate_t, __Reply__mach_port_allocate_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.right = right;
    result = rpc.simple(task, 3204);
    if (result == KERN_SUCCESS)
        *name = rpc.data.reply.name;
    return result;
}

kern_return_t mach_port_insert_right(ipc_space_t task, mach_port_name_t name, mach_port_t right, mach_msg_type_name_t type)
{
    auto result = static_cast<kern_return_t>(machTrap(-21, task, name, right, type));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    RPC<__Request__mach_port_insert_right_t, mig_reply_error_t> rpc;
    rpc.data.request.msgh_body.msgh_descriptor_count = 1;
    rpc.data.request.poly.name = right;
    rpc.data.request.poly.disposition = type;
    rpc.data.request.poly.type = MACH_MSG_PORT_DESCRIPTOR;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.name = name;
    return rpc.simple(task, 3214, sizeof(mig_reply_error_t), true);
}

kern_return_t mach_port_construct(ipc_space_t task, mach_port_options_ptr_t options, mach_port_context_t context, mach_port_name_t *name)
{
    auto result = static_cast<kern_return_t>(machTrap(-24, task, addressOf(options), context, addressOf(name)));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    if (!options || !name)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__mach_port_construct_t, __Reply__mach_port_construct_t> rpc;
    rpc.data.request.msgh_body.msgh_descriptor_count = 1;
    outOfLine(rpc.data.request.options, options, sizeof(*options));
    rpc.data.request.NDR = kNDR;
    rpc.data.request.context = context;
    result = rpc.simple(task, 3231, sizeof(rpc.data.reply), true);
    if (result == KERN_SUCCESS)
        *name = rpc.data.reply.name;
    return result;
}

kern_return_t mach_port_destruct(ipc_space_t task, mach_port_name_t name, mach_port_delta_t delta, mach_port_context_t guard)
{
    auto result = static_cast<kern_return_t>(machTrap(-25, task, name, delta, guard));
    if (result != MACH_SEND_INVALID_DEST)
        return result;
    RPC<__Request__mach_port_destruct_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.name = name;
    rpc.data.request.srdelta = delta;
    rpc.data.request.guard = guard;
    return rpc.simple(task, 3232);
}

mach_msg_return_t mach_msg(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t sendSize, mach_msg_size_t receiveSize,
                           mach_port_name_t receivePort, mach_msg_timeout_t timeout, mach_port_name_t notify)
{
    if (header && (options & MACH_RCV_MSG) && !(options & MACH_SEND_MSG) && (options & MACH_RCV_SYNC_WAIT))
        header->msgh_remote_port = notify;
    return message(header, uint32_t(options) | kSendMessageQueue, sendSize, receiveSize, receivePort, timeout, options & MACH_SEND_MSG ? notify : 0);
}

void mach_msg_destroy(mach_msg_header_t *header)
{
    if (!header || header->msgh_size < sizeof(*header))
        return;
    destroyRight(header->msgh_remote_port, MACH_MSGH_BITS_REMOTE(header->msgh_bits));
    destroyRight(header->msgh_voucher_port, MACH_MSGH_BITS_VOUCHER(header->msgh_bits));
    if (!(header->msgh_bits & MACH_MSGH_BITS_COMPLEX) || header->msgh_size < sizeof(mach_msg_base_t))
        return;
    auto *base = reinterpret_cast<mach_msg_base_t *>(header);
    auto *cursor = reinterpret_cast<uint8_t *>(base + 1);
    auto *end = reinterpret_cast<uint8_t *>(header) + header->msgh_size;
    for (mach_msg_size_t index = 0; index < base->body.msgh_descriptor_count; ++index) {
        if (static_cast<size_t>(end - cursor) < sizeof(mach_msg_type_descriptor_t))
            return;
        auto *descriptor = reinterpret_cast<mach_msg_descriptor_t *>(cursor);
        size_t size;
        switch (descriptor->type.type) {
            case MACH_MSG_PORT_DESCRIPTOR: size = sizeof(mach_msg_port_descriptor_t); break;
            case MACH_MSG_OOL_DESCRIPTOR:
            case MACH_MSG_OOL_VOLATILE_DESCRIPTOR: size = sizeof(mach_msg_ool_descriptor_t); break;
            case MACH_MSG_OOL_PORTS_DESCRIPTOR: size = sizeof(mach_msg_ool_ports_descriptor_t); break;
            case MACH_MSG_GUARDED_PORT_DESCRIPTOR: size = sizeof(mach_msg_guarded_port_descriptor_t); break;
            default: return;
        }
        if (static_cast<size_t>(end - cursor) < size)
            return;
        switch (descriptor->type.type) {
            case MACH_MSG_PORT_DESCRIPTOR:
                destroyRight(descriptor->port.name, descriptor->port.disposition);
                break;
            case MACH_MSG_OOL_DESCRIPTOR:
            case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
                if (descriptor->out_of_line.deallocate && descriptor->out_of_line.size)
                    (void)lsb::vm_deallocate(lsb::task_self_trap(), addressOf(descriptor->out_of_line.address), descriptor->out_of_line.size);
                break;
            case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
                auto &ports = descriptor->ool_ports;
                auto *names = static_cast<mach_port_t *>(ports.address);
                for (mach_msg_size_t i = 0; i < ports.count; ++i)
                    destroyRight(names[i], ports.disposition);
                if (ports.deallocate && ports.count)
                    (void)lsb::vm_deallocate(lsb::task_self_trap(), addressOf(names), size_t(ports.count) * sizeof(*names));
                break;
            }
            case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
                auto &port = descriptor->guarded_port;
                if (!(port.flags & MACH_MSG_GUARD_FLAGS_UNGUARDED_ON_SEND))
                    (void)machTrap(-42, lsb::task_self_trap(), port.name, port.context);
                destroyRight(port.name, port.disposition);
                break;
            }
            default: break;
        }
        cursor += size;
    }
}

kern_return_t task_info(task_name_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count)
{
    if (!info || !count)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__task_info_t, __Reply__task_info_t> rpc;
    const auto capacity = *count;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.flavor = flavor;
    rpc.data.request.task_info_outCnt = capacity;
    auto result = rpc.simple(task, 3405, offsetof(__Reply__task_info_t, task_info_out));
    if (result != KERN_SUCCESS)
        return result;
    auto returned = rpc.data.reply.task_info_outCnt;
    if (returned > sizeof(rpc.data.reply.task_info_out) / sizeof(integer_t) ||
        offsetof(__Reply__task_info_t, task_info_out) + size_t(returned) * sizeof(integer_t) > rpc.data.reply.Head.msgh_size)
        return MIG_TYPE_ERROR;
    *count = returned;
    lsb::copy(info, rpc.data.reply.task_info_out, size_t(returned < capacity ? returned : capacity) * sizeof(integer_t));
    return returned > capacity ? MIG_ARRAY_TOO_LARGE : KERN_SUCCESS;
}

kern_return_t thread_info(thread_inspect_t thread, thread_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count)
{
    if (!info || !count)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__thread_info_t, __Reply__thread_info_t> rpc;
    const auto capacity = *count;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.flavor = flavor;
    rpc.data.request.thread_info_outCnt = capacity;
    auto result = rpc.simple(thread, 3612, offsetof(__Reply__thread_info_t, thread_info_out));
    if (result != KERN_SUCCESS)
        return result;
    auto returned = rpc.data.reply.thread_info_outCnt;
    if (returned > sizeof(rpc.data.reply.thread_info_out) / sizeof(integer_t) ||
        offsetof(__Reply__thread_info_t, thread_info_out) + size_t(returned) * sizeof(integer_t) > rpc.data.reply.Head.msgh_size)
        return MIG_TYPE_ERROR;
    *count = returned;
    lsb::copy(info, rpc.data.reply.thread_info_out, size_t(returned < capacity ? returned : capacity) * sizeof(integer_t));
    return returned > capacity ? MIG_ARRAY_TOO_LARGE : KERN_SUCCESS;
}

kern_return_t task_register_dyld_image_infos(task_t task, dyld_kernel_image_info_array_t images, mach_msg_type_number_t count)
{
    return registerImages<__Request__task_register_dyld_image_infos_t>(task, images, count, 3444);
}

kern_return_t task_unregister_dyld_image_infos(task_t task, dyld_kernel_image_info_array_t images, mach_msg_type_number_t count)
{
    return registerImages<__Request__task_unregister_dyld_image_infos_t>(task, images, count, 3445);
}

kern_return_t task_get_dyld_image_infos(task_read_t task, dyld_kernel_image_info_array_t *images, mach_msg_type_number_t *count)
{
    if (!images || !count)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__task_get_dyld_image_infos_t, __Reply__task_get_dyld_image_infos_t> rpc;
    auto result = rpc.call(task, 3446);
    if (result != KERN_SUCCESS)
        return result;
    auto &reply = rpc.data.reply;
    if (!(reply.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) || reply.Head.msgh_size != sizeof(reply) ||
        reply.msgh_body.msgh_descriptor_count != 1 || reply.dyld_images.type != MACH_MSG_OOL_DESCRIPTOR ||
        uint64_t(reply.dyld_imagesCnt) * sizeof(**images) != reply.dyld_images.size) {
        lsb::mach_msg_destroy(&reply.Head);
        return MIG_TYPE_ERROR;
    }
    *images = static_cast<dyld_kernel_image_info_array_t>(reply.dyld_images.address);
    *count = reply.dyld_imagesCnt;
    return KERN_SUCCESS;
}

kern_return_t task_register_dyld_shared_cache_image_info(task_t task, dyld_kernel_image_info_t image, boolean_t noCache, boolean_t privateCache)
{
    RPC<__Request__task_register_dyld_shared_cache_image_info_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    lsb::copy(&rpc.data.request.dyld_cache_image, &image, sizeof(image));
    rpc.data.request.no_cache = noCache;
    rpc.data.request.private_cache = privateCache;
    return rpc.simple(task, 3447);
}

kern_return_t task_register_dyld_set_dyld_state(task_t task, uint8_t state)
{
    RPC<__Request__task_register_dyld_set_dyld_state_t, mig_reply_error_t> rpc;
    rpc.data.request.NDR = kNDR;
    rpc.data.request.dyld_state = state;
    return rpc.simple(task, 3448);
}

kern_return_t task_register_dyld_get_process_state(task_t task, dyld_kernel_process_info_t *state)
{
    if (!state)
        return KERN_INVALID_ARGUMENT;
    RPC<__Request__task_register_dyld_get_process_state_t, __Reply__task_register_dyld_get_process_state_t> rpc;
    auto result = rpc.simple(task, 3449);
    if (result == KERN_SUCCESS)
        lsb::copy(state, &rpc.data.reply.dyld_process_state, sizeof(*state));
    return result;
}

kern_return_t task_dyld_process_info_notify_get(mach_port_name_array_t names, natural_t *count)
{
    return static_cast<kern_return_t>(machTrap(-13, addressOf(names), addressOf(count)));
}

} // namespace lsb
