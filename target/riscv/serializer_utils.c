#include "checkpoint/checkpoint.h"
#include "hw/boards.h"
#include "hw/riscv/nemu.h"
#include "checkpoint/serializer_utils.h"
#include <zstd.h>

#define USE_ZSTD_COMPRESS
void serialize_pmem(uint64_t inst_count)
{

    MachineState *ms = MACHINE(qdev_get_machine());
    NEMUState *ns=NEMU_MACHINE(ms);
    uint64_t guest_pmem_size=ms->ram_size;
    char* pmem_addr=ns->memory;

    assert(pmem_addr);

#define FILEPATH_BUF_SIZE 256
    char filepath[FILEPATH_BUF_SIZE];

    //prepare path
    if (checkpoint.checkpoint_mode == SimpointCheckpointing) {
        strcpy(filepath,((GString*)(g_list_first(path_manager.checkpoint_path_list)->data))->str);
    }else if(checkpoint.checkpoint_mode==UniformCheckpointing){
        sprintf(filepath, "%s/%ld/_%ld_.gz" ,path_manager.uniform_path->str,inst_count,inst_count);
    }

    printf("prepare for generate checkpoint path %s pmem_size %ld\n",filepath,guest_pmem_size);
    assert(g_mkdir_with_parents(g_path_get_dirname(filepath), 0775)==0);

#ifdef USE_ZSTD_COMPRESS
    //zstd compress
    size_t const compress_buffer_size = ZSTD_compressBound(guest_pmem_size);
    void* const compress_buffer = malloc(compress_buffer_size);
    assert(compress_buffer);

    size_t const compress_size = ZSTD_compress(compress_buffer, compress_buffer_size, pmem_addr, guest_pmem_size, 1);
    assert(compress_size<=compress_buffer_size&&compress_size!=0);

    FILE *compress_file=fopen(filepath,"wb");
    size_t fw_size = fwrite(compress_buffer,1,compress_size,compress_file);

    if (fw_size != (size_t)compress_size) {
        fprintf(stderr, "fwrite: %s : %s \n", filepath, strerror(errno));
    }
    if (fclose(compress_file)) {
        fprintf(stderr, "fclose: %s : %s \n", filepath, strerror(errno));
    }

    free(compress_buffer);

#endif

#ifdef USE_ZLIB_COMPRESS
    //zlib compress
    gzFile compressed_mem=NULL;
    compressed_mem=gzopen(filepath,"wb");

    if (!compressed_mem) {
        error_printf("filename %s can't open", filepath);
        return;
    }

    uint64_t write_size;
    uint64_t seg_size=1*1024*1024*1024;
    for (int i = 0;i<guest_pmem_size/seg_size;i++) {
        write_size=gzwrite(compressed_mem, pmem_addr+(i*seg_size),seg_size);
        printf("wirte in index %d\n",i);
        if (write_size != seg_size) {
            error_printf("qmp_gzpmemsave write error size %ld index %d\n",write_size,i);
            goto exit;
        }
    }
    info_report("success write into checkpoint file: %s",filepath);
exit:
    gzclose(compressed_mem);
#endif
    //    useless for now
    //    uint64_t mtime;
    //    cpu_physical_memory_read(MTIME_CMP_CPT_ADDR, &mtime, 8);
    //    cpu_physical_memory_write(CLINT_MMIO+CLINT_MTIME, &mtime, 8);
}
