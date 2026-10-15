#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <zlib.h>
#include "../../../3rdparty/miniz/miniz.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main()
{
    const char payload[] = "Sven Co-op depot manifest and chunk data";
    assert(crc32(0, reinterpret_cast<const Bytef *>(payload), sizeof(payload)) ==
        mz_crc32(0, reinterpret_cast<const unsigned char *>(payload), sizeof(payload)));
    for (unsigned compression : {0u, 6u})
    {
        mz_zip_archive writer = {};
        assert(mz_zip_writer_init_heap(&writer, 0, 0));
        assert(mz_zip_writer_add_mem(&writer, "manifest", payload, sizeof(payload), compression));
        void *archive = nullptr;
        size_t archiveSize = 0;
        assert(mz_zip_writer_finalize_heap_archive(&writer, &archive, &archiveSize));
        assert(mz_zip_writer_end(&writer));

        mz_zip_archive reader = {};
        assert(mz_zip_reader_init_mem(&reader, archive, archiveSize, 0));
        assert(mz_zip_reader_get_num_files(&reader) == 1);
        mz_zip_archive_file_stat entry = {};
        assert(mz_zip_reader_file_stat(&reader, 0, &entry));
        assert(entry.m_uncomp_size == sizeof(payload));
        size_t extractedSize = 0;
        void *extracted = mz_zip_reader_extract_to_heap(&reader, 0, &extractedSize, 0);
        assert(extracted && extractedSize == sizeof(payload));
        assert(std::memcmp(extracted, payload, sizeof(payload)) == 0);
        mz_free(extracted);
        assert(mz_zip_reader_end(&reader));
        mz_free(archive);
    }
    mz_zip_archive invalid = {};
    assert(!mz_zip_reader_init_mem(&invalid, payload, sizeof(payload), 0));
    std::cout << "PASS: Miniz C/C++ linkage, stored/deflated Steam ZIP extraction and invalid archive rejection\n";
}
