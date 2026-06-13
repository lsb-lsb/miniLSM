#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <io.h>
#include "minilsm/sstable.h"
#include "minilsm/sstable_builder.h"
#include "minilsm/coding.h"

int main() {
    using namespace minilsm;
    const char* path = "debug_io_test.sst";
    std::remove(path);

    SSTableBuilder builder(path);
    builder.Add(Slice("debug_key_000000"), Slice("debug_val_000000"));
    builder.Add(Slice("first_key"), Slice("value_1"));
    builder.Add(Slice("second_key"), Slice("value_2"));
    builder.Add(Slice("third_key"), Slice("value_3"));
    Status s = builder.Finish();
    printf("Finish: %s (file_size=%zu)\n", s.ToString().c_str(), builder.FileSize());

    SSTable* table = nullptr;
    s = SSTable::Open(path, &table);
    printf("SSTable::Open: %s\n", s.ToString().c_str());
    if (table) {
        printf("  FirstKey=%s LastKey=%s\n", table->FirstKey().ToString().c_str(), table->LastKey().ToString().c_str());
        printf("  Index entries: %zu\n", table->index().size());
        for (size_t i = 0; i < table->index().size(); i++) {
            auto& e = table->index()[i];
            printf("    [%zu] key=%s offset=%llu size=%llu\n", i, e.last_key.c_str(),
                   (unsigned long long)e.block_offset, (unsigned long long)e.block_size);
        }
        table->Unref();
    }

    std::remove("temp_corrupt.sst");
    {
        SSTableBuilder b2("temp_corrupt.sst");
        b2.Add(Slice("corrupt_key_000000"), Slice("corrupt_value_000000"));
        b2.Finish();
    }

    FILE* f = fopen("temp_corrupt.sst", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        printf("\nTemp file size: %ld\n", sz);

        fseek(f, sz - 48, SEEK_SET);
        char footer[48];
        size_t n = fread(footer, 1, 48, f);
        printf("fread footer (abs): %zu bytes, magic=0x%016llX\n", n,
               (unsigned long long)DecodeFixed64(footer + 32));

        fseek(f, sz - 8, SEEK_SET);
        char magic_buf[8] = {0};
        n = fread(magic_buf, 1, 8, f);
        printf("fread last 8 (abs seek): %zu bytes, magic=0x%016llX, bytes=[%02X%02X%02X%02X %02X%02X%02X%02X]\n", n,
               (unsigned long long)DecodeFixed64(magic_buf),
               (unsigned char)magic_buf[0],(unsigned char)magic_buf[1],(unsigned char)magic_buf[2],(unsigned char)magic_buf[3],
               (unsigned char)magic_buf[4],(unsigned char)magic_buf[5],(unsigned char)magic_buf[6],(unsigned char)magic_buf[7]);
        fclose(f);
    }

    int fd = open("temp_corrupt.sst", O_RDONLY | O_BINARY);
    if (fd >= 0) {
        struct stat st;
        fstat(fd, &st);
        printf("\nPOSIX st_size: %lld\n", (long long)st.st_size);
        off_t pos = lseek(fd, -8, SEEK_END);
        printf("POSIX lseek to -8: pos=%lld\n", (long long)pos);
        char mb[8] = {0};
        ssize_t r = _read(fd, mb, 8);
        printf("POSIX _read: %zd bytes, bytes=[%02X%02X%02X%02X %02X%02X%02X%02X], magic=0x%016llX\n",
               r,
               (unsigned char)mb[0],(unsigned char)mb[1],(unsigned char)mb[2],(unsigned char)mb[3],
               (unsigned char)mb[4],(unsigned char)mb[5],(unsigned char)mb[6],(unsigned char)mb[7],
               (unsigned long long)DecodeFixed64(mb));
        close(fd);
    }

    f = fopen("temp_corrupt.sst", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        char* buf = new char[sz];
        fseek(f, 0, SEEK_SET);
        fread(buf, 1, sz, f);
        fclose(f);
        printf("\nFull hex dump:\n");
        for (long i = 0; i < sz; i++) {
            if (i % 16 == 0) printf("%04ld: ", i);
            printf("%02X ", (unsigned char)buf[i]);
            if (i % 16 == 15) printf("\n");
        }
        if (sz % 16 != 0) printf("\n");
        delete[] buf;
    }

    std::remove(path);
    std::remove("temp_corrupt.sst");
    return 0;
}
