

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

pid_t pid = getpid();
struct proc_args {
    unsigned int block_size;
    unsigned long disk_blocks;
    unsigned long n_ops;
    int pct_writes;
};

void io_worker(const struct proc_args *args) {
    unsigned int block_size = args->block_size;
    unsigned long disk_blocks = args->disk_blocks;
    unsigned long n_ops = args->n_ops;
    int pct_writes = args->pct_writes;

    printf("[io_worker] %d com %lu operações\n", (int)getpid(), n_ops);

    int fd = open("/dev/sdb", O_RDWR| O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open the device");
        _exit(EXIT_FAILURE);
    }

    char *buf_r = malloc(block_size);
    if (!buf_r) {
        perror("malloc buf_r");
        close(fd);
        _exit(EXIT_FAILURE);
    }
    memset(buf_r, 0xAA, block_size);

    char *buf = malloc(block_size);
    if (!buf) {
        perror("malloc buf");
        free(buf_r);
        close(fd);
        _exit(EXIT_FAILURE);
    }
    strcpy(buf, "Hello World!");

    unsigned int seed = (unsigned int)getpid();

    for (unsigned long i = 0; i < n_ops; i++) {
        unsigned long blk = rand_r(&seed) % disk_blocks;
        off_t offset = (off_t)blk * block_size;

        int op = (rand_r(&seed) % 100) < pct_writes;

        if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            break;
        }

        if (op) {
            if (write(fd, buf, block_size) != (ssize_t)block_size) {
                perror("write");
                break;
            }
            printf("pid %d -> write block %lu\n", getpid(), blk);
        } else {
            if (read(fd, buf_r, block_size) != (ssize_t)block_size) {
                perror("read");
                break;
            }
            printf("pid %d -> read  block %lu\n", getpid(), blk);
        }

    }
    free(buf);
    free(buf_r);
    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc != 6) {
        fprintf(stderr,
            "Uso: %s <tamanho_bloco_bytes> <tamanho_disco_blocos> \
            \n <num_operacoes> <pct_escritas> <num_processos>\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned int block_size = atoi(argv[1]);
    unsigned long disk_blocks = strtoul(argv[2], NULL, 10);
    unsigned long n_ops = strtoul(argv[3], NULL, 10);
    int pct_writes = atoi(argv[4]);
    int num_procs = atoi(argv[5]);

    if (block_size <= 1 || disk_blocks <= 1 || n_ops <= 1 || pct_writes < 0 || pct_writes > 100 || num_procs <= 1) {
        fprintf(stderr, "Parâmetros inválidos!\n");
        return EXIT_FAILURE;
    }

    unsigned long disk_size_bytes = disk_blocks * (unsigned long)block_size;

    printf("Parâmetros:\n");
    printf("  Block size:       %u bytes\n", block_size);
    printf("  Disk size:        %lu blocks (%lu bytes)\n", disk_blocks, disk_size_bytes);
    printf("  # operações:      %lu\n", n_ops);
    printf("  %% de escritas:    %d%%\n", pct_writes);
    printf("  Processos:        %d\n", num_procs);

    printf("Cleaning disk cache...\n");
    if (system("echo 3 > /proc/sys/vm/drop_caches") != 0) {
        fprintf(stderr, "Falha ao limpar cache\n");
    }

    printf("Configuring scheduling queues...\n");
    system("echo 2 > /sys/block/sdb/queue/nomerges");
    system("echo 4 > /sys/block/sdb/queue/max_sectors_kb");
    system("echo 0 > /sys/block/sdb/queue/read_ahead_kb");

   unsigned long base_ops = n_ops / num_procs;
    unsigned long extra    = n_ops % num_procs;

    for (int i = 0; i < num_procs; i++) {
        unsigned long ops_for_this = base_ops + (i < extra ? 1 : 0);
        struct proc_args args_i = {
            .block_size  = block_size,
            .disk_blocks = disk_blocks,
            .n_ops       = ops_for_this,
            .pct_writes  = pct_writes
        };
        if (fork() == 0) {
            io_worker(&args_i);
            _exit(EXIT_SUCCESS);
        }
    }

    /* Pai espera todos os filhos */
    for (int i = 0; i < num_procs; i++) {
        wait(NULL);
    }

    return 0;
}
