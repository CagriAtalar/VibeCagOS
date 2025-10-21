#include "common.h"

void putchar(char ch);  // Forward declaration

// Embedded files - compiled directly into kernel
extern char _binary_disk_hello_txt_start[];
extern char _binary_disk_hello_txt_end[];
extern char _binary_disk_hello_txt_size[];

extern char _binary_disk_meow_txt_start[];
extern char _binary_disk_meow_txt_end[];
extern char _binary_disk_meow_txt_size[];

#define MAX_FILES 10
#define MAX_FILE_SIZE 2048

// Runtime file structure (in RAM)
struct ram_file {
    bool in_use;
    char name[64];
    char data[MAX_FILE_SIZE];
    size_t size;
    bool is_embedded;  // true if it's an embedded file
    char *embedded_start;
};

static struct ram_file ram_files[MAX_FILES];

// Initialize with embedded files
struct embedded_file_info {
    const char *name;
    char *start;
    char *end;
};

struct embedded_file_info embedded_files[] = {
    {"hello.txt", _binary_disk_hello_txt_start, _binary_disk_hello_txt_end},
    {"meow.txt", _binary_disk_meow_txt_start, _binary_disk_meow_txt_end},
    {NULL, NULL, NULL}
};

void embedded_fs_init(void) {
    // Clear RAM files
    for (int i = 0; i < MAX_FILES; i++) {
        ram_files[i].in_use = false;
    }
    
    printf("Embedded file system initialized (RAM-based)\n");
    
    // Load embedded files into RAM
    int file_idx = 0;
    for (int i = 0; embedded_files[i].name != NULL && file_idx < MAX_FILES; i++) {
        size_t size = embedded_files[i].end - embedded_files[i].start;
        
        ram_files[file_idx].in_use = true;
        ram_files[file_idx].is_embedded = true;
        ram_files[file_idx].embedded_start = embedded_files[i].start;
        ram_files[file_idx].size = size;
        strcpy(ram_files[file_idx].name, embedded_files[i].name);
        
        printf("  %s (%d bytes) [embedded]\n", ram_files[file_idx].name, ram_files[file_idx].size);
        file_idx++;
    }
}

static struct ram_file *fs_lookup(const char *filename) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use && strcmp(ram_files[i].name, filename) == 0) {
            return &ram_files[i];
        }
    }
    return NULL;
}

void embedded_fs_list(void) {
    printf("Files:\n");
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ram_files[i].in_use) {
            printf("  %s (%d bytes)%s\n", 
                   ram_files[i].name, 
                   ram_files[i].size,
                   ram_files[i].is_embedded ? " [embedded]" : " [created]");
            count++;
        }
    }
    if (count == 0) {
        printf("  (no files)\n");
    }
}

void embedded_fs_cat(const char *filename) {
    struct ram_file *file = fs_lookup(filename);
    if (!file) {
        printf("File not found: %s\n", filename);
        return;
    }
    
    if (file->is_embedded) {
        // Read from embedded location
        for (size_t i = 0; i < file->size; i++) {
            putchar(file->embedded_start[i]);
        }
    } else {
        // Read from RAM buffer
        for (size_t i = 0; i < file->size; i++) {
            putchar(file->data[i]);
        }
    }
    putchar('\n');
}

int embedded_fs_create(const char *filename) {
    // Check if already exists
    if (fs_lookup(filename)) {
        return -1;  // Already exists
    }
    
    // Find free slot
    for (int i = 0; i < MAX_FILES; i++) {
        if (!ram_files[i].in_use) {
            ram_files[i].in_use = true;
            ram_files[i].is_embedded = false;
            ram_files[i].size = 0;
            strcpy(ram_files[i].name, filename);
            memset(ram_files[i].data, 0, MAX_FILE_SIZE);
            return 0;
        }
    }
    
    return -2;  // No space
}

int embedded_fs_write(const char *filename, const char *content, size_t len) {
    struct ram_file *file = fs_lookup(filename);
    if (!file) {
        return -1;  // Not found
    }
    
    if (len > MAX_FILE_SIZE) {
        len = MAX_FILE_SIZE;
    }
    
    // If embedded, need to copy to RAM first
    if (file->is_embedded) {
        // Copy embedded content to RAM buffer
        size_t copy_size = file->size < MAX_FILE_SIZE ? file->size : MAX_FILE_SIZE;
        for (size_t i = 0; i < copy_size; i++) {
            file->data[i] = file->embedded_start[i];
        }
        file->is_embedded = false;
    }
    
    // Write new content
    for (size_t i = 0; i < len; i++) {
        file->data[i] = content[i];
    }
    file->size = len;
    
    return len;
}

int embedded_fs_delete(const char *filename) {
    struct ram_file *file = fs_lookup(filename);
    if (!file) {
        return -1;  // Not found
    }
    
    file->in_use = false;
    return 0;
}

