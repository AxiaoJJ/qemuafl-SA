extern "C" {
#include <qemu/qemu-plugin.h>
}

#include <dlfcn.h>
#include <string>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

using namespace std;

typedef bool (*arch_supported_fn)(const char *);
typedef bool (*is_indirect_branch_fn)(uint8_t *, size_t);

arch_supported_fn arch_supported;
is_indirect_branch_fn is_indirect_branch;

// Address of previous callsite if it was an indirect jump/call
static optional<uint64_t> branch_addr = {};

static ofstream outfile;

typedef struct image_offset {
    // An offset into a loaded ELF file
    uint64_t offset;
    // The position of the image name in the /proc/self/maps entry for the
    // corresponding offset.
    size_t image_name_pos;
} image_offset;

// Check if the given `guest_vaddr` falls within the loaded segment described
// by the `maps_entry` which is a line read from /proc/self/maps.
//计算偏移值
static optional<image_offset> guest_vaddr_to_offset(const string_view maps_entry, uint64_t guest_vaddr) {
    uint32_t name_pos;
    uint64_t start, end, file_load_offset;

    // QEMU may add a constant offset to the emulated system's memory. Adding
    // guest base to guest_vaddr converts it back to a "host" vaddr that can
    // be compared against the host system's vaddrs in /proc/self/maps
    uint64_t host_vaddr = guest_vaddr + qemu_plugin_guest_base();

    // Parse the /proc/self/maps line. Stores the start and end vaddrs of the
    // loaded segment, the offset into the file the segment was loaded from
    // (`file_load_offset`) and the number of characters in the maps string
    // before the name of the ELF file (`name_pos`).
    sscanf(maps_entry.data(), "%lx-%lx %*c%*c%*c%*c %lx %*lx:%*lx %*lu %n", &start, &end, &file_load_offset,
           &name_pos);
    // Check if the addr is within the segment. Comparing host_vaddr with the
    // maps vaddrs is ok since it's also a system vaddr
    if ((start <= host_vaddr) && (host_vaddr <= end)) {
        // Get the address as an offset into the loaded segment
        uint64_t segment_offset = host_vaddr - start;
        // Turn the segment offset into an offset into the file
        uint64_t file_offset = segment_offset + file_load_offset;
        struct image_offset offset = {
            .offset = file_offset,
            .image_name_pos = name_pos,
        };
        return offset;
    }
    return {};
}

// Write the destination of an indirect jump/call to the output file
// 记录间接分支的源地址和目标地址
static void mark_indirect_branch(uint64_t callsite_vaddr, uint64_t dst_vaddr) {
    ifstream maps("/proc/self/maps");
    string line;
    optional<image_offset> callsite = {};
    optional<image_offset> dst = {};
    string callsite_image = "";
    string dst_image = "";
    // For each entry in /proc/self/maps
    while (getline(maps, line)) {
        if (!callsite.has_value()) {
            callsite = guest_vaddr_to_offset(line, callsite_vaddr);
            if (callsite.has_value()) {
                // Copy name since `line` gets reused in this loop
                char *image_name = line.data() + callsite->image_name_pos;
                callsite_image = string(image_name);
            }
        }
        if (!dst.has_value()) {
            dst = guest_vaddr_to_offset(line, dst_vaddr);
            if (dst.has_value()) {
                // Copy name since `line` gets reused in this loop
                char *image_name = line.data() + dst->image_name_pos;
                dst_image = string(image_name);
            }
        }
        // Skip the remaining entries after finding the callsite and destination
        if (callsite.has_value() && dst.has_value()) {
            break;
        }
    }
    if (!callsite.has_value()) {
        cout << "ERROR: Unable to find callsite address in /proc/self/maps" << endl;
    }
    if (!dst.has_value()) {
        cout << "ERROR: Unable to find destination address in /proc/self/maps" << endl;
    }
    outfile << "0x" << hex << callsite->offset << ",";
    outfile << "0x" << hex << dst->offset << ",";
    outfile << "0x" << hex << callsite_vaddr << ",";
    outfile << "0x" << hex << dst_vaddr << ",";
    outfile << callsite_image << ",";
    outfile << dst_image << endl;
    return;
};

// Callback for insn at the start of a block
static void branch_taken(unsigned int vcpu_idx, void *dst_vaddr) {
    if (branch_addr.has_value()) {
        mark_indirect_branch(branch_addr.value(), (uint64_t)dst_vaddr);
        branch_addr = {};
    }
}

// Callback for insn following an indirect branch
//不是间接跳转指令不打印
static void branch_skipped(unsigned int vcpu_idx, void *userdata) { branch_addr = {}; }

// Callback for indirect branch insn
static void indirect_branch_exec(unsigned int vcpu_idx, void *callsite_addr) {
    branch_addr = (uint64_t)callsite_addr;
}

// Callback for indirect branch which may also be the destination of another branch
static void indirect_branch_at_start(unsigned int vcpu_idx, void *callsite_addr) {
    branch_taken(vcpu_idx, callsite_addr);
    indirect_branch_exec(vcpu_idx, callsite_addr);
}

// Register a callback for each time a block is executed
// 是对每个基本块执行回调函数
static void block_trans_handler(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    uint64_t start_vaddr = qemu_plugin_tb_vaddr(tb);
    size_t num_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < num_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t insn_addr = qemu_plugin_insn_vaddr(insn);  //获取块中每条指令的虚拟地址

        uint8_t *insn_data = (uint8_t *)qemu_plugin_insn_data(insn);
        size_t insn_size = qemu_plugin_insn_size(insn);

        bool insn_is_branch = is_indirect_branch(insn_data, insn_size);
        // The callback for the first instruction in a block should mark the indirect branch
        // destination if one was taken
        if (i == 0) {
            if (!insn_is_branch) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, branch_taken, QEMU_PLUGIN_CB_NO_REGS,
                                                       (void *)start_vaddr);        //检测是不是间接调用跳转过来的
            } else {
                // If the first branch is also an indirect branch, the callback must mark the
                // destination and update `branch_addr`
                qemu_plugin_register_vcpu_insn_exec_cb(insn, indirect_branch_at_start,
                                                       QEMU_PLUGIN_CB_NO_REGS, (void *)start_vaddr);
                // In this case the second insn should clear `branch_addr` like below
                if (num_insns > 1) {
                    struct qemu_plugin_insn *next_insn = qemu_plugin_tb_get_insn(tb, 1);
                    qemu_plugin_register_vcpu_insn_exec_cb(next_insn, branch_skipped,
                                                           QEMU_PLUGIN_CB_NO_REGS, NULL);
                }
            }
        } else {
            if (insn_is_branch) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, indirect_branch_exec,
                                                       QEMU_PLUGIN_CB_NO_REGS, (void *)insn_addr);
                if (i + 1 < num_insns) {
                    struct qemu_plugin_insn *next_insn = qemu_plugin_tb_get_insn(tb, i + 1);
                    uint8_t *next_data = (uint8_t *)qemu_plugin_insn_data(next_insn);
                    size_t next_size = qemu_plugin_insn_size(next_insn);
                    if (is_indirect_branch(next_data, next_size)) {
                        cout << "WARNING: Consecutive indirect branches are currently not handled properly" << endl;
                    }
                    qemu_plugin_register_vcpu_insn_exec_cb(next_insn, branch_skipped,
                                                           QEMU_PLUGIN_CB_NO_REGS, NULL);
                }
            }
        }
    }
}

int loading_sym_failed(const char *sym, const char *backend_name) {
    cout << "Could not load `" << sym << "` function from backend " << backend_name << endl;
    cout << dlerror() << endl;
    return -4;
}

extern int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc,
                               char **argv) {
    /*if (argc < 1) {
        cout << "Usage: /path/to/qemu \\" << endl;
        cout << "\t-plugin /path/to/libibresolver.so,output=\"output.csv\",backend=\"/path/to/disassembly/libbackend.so\" \\" << endl;
        cout << "\t$BINARY" << endl;
        return -1;
    }*/

    //const char *output_arg = argv[0] + sizeof("output=") - 1;

    outfile = ofstream("output.csv");
    if (outfile.fail()) {
        cout << "Could not open file output.csv" << endl;
        return -2;
    }

    bool backend_provided = argc == 2;
    void *backend_handle = RTLD_DEFAULT;
    const char *arch_supported_fn_name = "arch_supported_default_impl";
    const char *is_indirect_branch_fn_name = "is_indirect_branch_default_impl";
    const char *backend_name = BACKEND_NAME;

    if (backend_provided) {
        const char *backend_arg = argv[1] + sizeof("backend=") - 1;     
        backend_handle = dlopen(backend_arg, RTLD_LAZY | RTLD_DEEPBIND);    //打开后端共享库
        if (!backend_handle) {
            cout << "Could not open shared library for alternate disassembly backend" << endl;
            cout << dlerror() << endl;
            return -3;
        }
        arch_supported_fn_name = "arch_supported";
        is_indirect_branch_fn_name = "is_indirect_branch";
        backend_name = backend_arg;
    }
    cout << "Using the " << backend_name << " disassembly backend" << endl;
    arch_supported = (arch_supported_fn)dlsym(backend_handle, arch_supported_fn_name);
    if (dlerror()) {
        return loading_sym_failed(arch_supported_fn_name, backend_name);
    }
    is_indirect_branch = (is_indirect_branch_fn)dlsym(backend_handle, is_indirect_branch_fn_name);  //从动态加载库种获取is_indirect_branch_fn_name函数的地址
    if (dlerror()) {
        return loading_sym_failed(is_indirect_branch_fn_name, backend_name);
    }

    if (!arch_supported(info->target_name)) {
        cout << "Could not initialize disassembly backend for " << info->target_name << endl;
        return -5;
    }

    outfile << "callsite offset,dest offset,callsite vaddr,dest vaddr,callsite ELF,dest ELF" << endl;
    // Register a callback for each time a block is translated
    qemu_plugin_register_vcpu_tb_trans_cb(id, block_trans_handler);

    return 0;
}
