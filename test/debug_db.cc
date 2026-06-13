#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <io.h>

int main() {
    const char* dir = "./test_db_debug";
    mkdir(dir);
    printf("mkdir(%s) result: errno=%d\n", dir, errno);
    
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, "000001.sst");
    printf("Trying fopen(%s, \"wb\")\n", path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("fopen FAILED: errno=%d (%s)\n", errno, strerror(errno));
    } else {
        printf("fopen OK\n");
        fwrite("test", 1, 4, f);
        fclose(f);
        printf("File written and closed\n");
    }
    
    // Check if file exists
    struct stat st;
    int ret = stat(path, &st);
    printf("stat(%s): ret=%d, errno=%d, size=%lld\n", path, ret, errno, (long long)(ret == 0 ? st.st_size : -1));
    
    // Try with backslashes
    char path2[256];
    snprintf(path2, sizeof(path2), "%s\%s", dir, "000002.sst");
    printf("\nTrying fopen(%s, \"wb\")\n", path2);
    f = fopen(path2, "wb");
    if (!f) {
        printf("fopen FAILED: errno=%d (%s)\n", errno, strerror(errno));
    } else {
        printf("fopen OK\n");
        fwrite("test", 1, 4, f);
        fclose(f);
    }
    
    // List directory
    printf("\nDirectory listing:\n");
    // Use ls equivalent
    rmdir(dir);
    printf("Cleaned up\n");
    return 0;
}
