#include "kernel.h"
#include "common.h"

// User space shell implementation
void user_shell(void) {
    printf("\n=== User Space Shell ===\n");
    printf("Welcome to user mode!\n");
    printf("Type 'help' for available commands\n\n");
    
    while (1) {
        printf("user@os$ ");
        char cmdline[128];
        int i = 0;
        
        // Read command with backspace support
        while (i < (int)sizeof(cmdline) - 1) {
            long ch = syscall(SYS_GETCHAR, 0, 0, 0);
            if (ch >= 0) {
                if (ch == '\r' || ch == '\n') {
                    printf("\n");
                    cmdline[i] = '\0';
                    break;
                }
                else if (ch == '\b' || ch == 127) {  // Backspace or DEL key
                    if (i > 0) {  // Only if not at beginning of prompt
                        i--;  // Move cursor back
                        printf("\b \b");  // Erase character
                    }
                }
                else {
                    printf("%c", (char)ch);
                    cmdline[i++] = (char)ch;
                }
            }
        }
        cmdline[i] = '\0';
        
        // Parse and execute commands
        if (strcmp(cmdline, "hello") == 0) {
            printf("Hello from user space!\n");
        }
        else if (strcmp(cmdline, "ls") == 0) {
            // Use syscall to list files
            char files[1024];
            int ret = syscall(SYS_LIST_FILES, (uint32_t)files, sizeof(files), 0);
            if (ret >= 0) {
                printf("Files:\n%s\n", files);
            } else {
                printf("Error listing files\n");
            }
        }
        else if (strncmp(cmdline, "cat ", 4) == 0) {
            char *filename = cmdline + 4;
            char content[2048];
            int ret = syscall(SYS_READ_FILE, (uint32_t)filename, (uint32_t)content, sizeof(content));
            if (ret >= 0) {
                printf("File '%s' contents:\n%s\n", filename, content);
            } else {
                printf("Error: File '%s' not found\n", filename);
            }
        }
        else if (strncmp(cmdline, "create ", 7) == 0) {
            char *filename = cmdline + 7;
            int ret = syscall(SYS_CREATE_FILE, (uint32_t)filename, 0, 0);
            if (ret == 0) {
                printf("File '%s' created\n", filename);
            } else {
                printf("Error creating file '%s'\n", filename);
            }
        }
        else if (strncmp(cmdline, "write ", 6) == 0) {
            char *filename = cmdline + 6;
            printf("Enter content (empty line to finish):\n");
            
            char content[2000];
            int content_len = 0;
            
            while (content_len < 1999) {
                printf("| ");
                char line[128];
                int line_len = 0;
                
                while (line_len < 127) {
                    long ch = syscall(SYS_GETCHAR, 0, 0, 0);
                    if (ch >= 0) {
                        if (ch == '\r' || ch == '\n') {
                            printf("\n");
                            line[line_len] = '\0';
                            break;
                        }
                        else if (ch == '\b' || ch == 127) {  // Backspace or DEL key
                            if (line_len > 0) {  // Only if not at beginning of line
                                line_len--;  // Move cursor back
                                printf("\b \b");  // Erase character
                            }
                        }
                        else {
                            printf("%c", (char)ch);
                            line[line_len++] = (char)ch;
                        }
                    }
                }
                line[line_len] = '\0';
                
                // Empty line ends input
                if (line_len == 0) {
                    break;
                }
                
                // Add line to content
                if (content_len > 0) {
                    content[content_len++] = '\n';
                }
                for (int j = 0; j < line_len; j++) {
                    content[content_len++] = line[j];
                }
            }
            content[content_len] = '\0';
            
            int ret = syscall(SYS_WRITE_FILE, (uint32_t)filename, (uint32_t)content, content_len);
            if (ret == 0) {
                printf("File '%s' written successfully\n", filename);
            } else {
                printf("Error writing to file '%s'\n", filename);
            }
        }
        else if (strncmp(cmdline, "rm ", 3) == 0) {
            char *filename = cmdline + 3;
            int ret = syscall(SYS_DELETE_FILE, (uint32_t)filename, 0, 0);
            if (ret == 0) {
                printf("File '%s' deleted\n", filename);
            } else {
                printf("Error: File '%s' not found\n", filename);
            }
        }
        else if (strcmp(cmdline, "format") == 0) {
            printf("Are you sure? This will erase all data! Type 'yes' to confirm: ");
            char confirm[10];
            int j = 0;
            while (j < 9) {
                long ch = syscall(SYS_GETCHAR, 0, 0, 0);
                if (ch >= 0) {
                    if (ch == '\r' || ch == '\n') {
                        printf("\n");
                        confirm[j] = '\0';
                        break;
                    }
                    else if (ch == '\b' || ch == 127) {  // Backspace or DEL key
                        if (j > 0) {  // Only if not at beginning
                            j--;  // Move cursor back
                            printf("\b \b");  // Erase character
                        }
                    }
                    else {
                        printf("%c", (char)ch);
                        confirm[j++] = (char)ch;
                    }
                }
            }
            confirm[j] = '\0';
            if (strcmp(confirm, "yes") == 0) {
                int ret = syscall(SYS_FORMAT_FS, 0, 0, 0);
                if (ret == 0) {
                    printf("Filesystem formatted\n");
                } else {
                    printf("Error formatting filesystem\n");
                }
            } else {
                printf("Format cancelled.\n");
            }
        }
        else if (strcmp(cmdline, "help") == 0) {
            printf("Available commands:\n");
            printf("  hello           - Print greeting\n");
            printf("  ls              - List all files\n");
            printf("  cat <file>      - Display file contents\n");
            printf("  create <file>   - Create new file\n");
            printf("  write <file>    - Write content to file\n");
            printf("  rm <file>       - Delete file\n");
            printf("  format          - Format filesystem\n");
            printf("  help            - Show this help\n");
            printf("  exit            - Exit to kernel\n");
        }
        else if (strcmp(cmdline, "exit") == 0) {
            printf("Exiting to kernel...\n");
            break;
        }
        else if (cmdline[0] != '\0') {
            printf("Unknown command: %s\n", cmdline);
            printf("Type 'help' for available commands\n");
        }
    }
}

// Enter user mode function
void enter_user_mode(void) {
    printf("Switching to user mode...\n");
    
    // Initialize user space
    user_shell();
    
    printf("Returned to kernel mode\n");
}
