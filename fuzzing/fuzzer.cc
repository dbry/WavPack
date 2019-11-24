#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vector>
#include <fstream>

#include "wavpack.h"
#include "md5.h"

using namespace std;


extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    //WavpackContext *wpc = WavpackOpenRawDecoder((void *)data, size, 0, 0, 0, 0, 0, 0);
    ofstream myFile ("/tmp/data.bin", ios::out | ios::binary);
    
    myFile.write ((char*)data, size);
    myFile.close ();
    char error [80];
    WavpackContext *wpc = WavpackOpenFileInput ("/tmp/data.bin", error, OPEN_WVC | OPEN_DSD_NATIVE | OPEN_ALT_TYPES, 0);

    char md5_string1 [] = "????????????????????????????????";
    char md5_string2 [] = "????????????????????????????????";
    int32_t *decoded_samples, num_chans, bps, test_index, qmode, total_samples;
    unsigned char md5_initial [16], md5_stored [16];
    MD5_CTX md5_global, md5_local;
    unsigned char *chunked_md5;

    if (!wpc) {
        remove ("/tmp/data.bin");
        return 1;
    }

    num_chans = WavpackGetNumChannels (wpc);
    total_samples = WavpackGetNumSamples64 (wpc);
    bps = WavpackGetBytesPerSample (wpc);
    qmode = WavpackGetQualifyMode (wpc);

    WavpackCloseFile (wpc);
    remove ("/tmp/data.bin");

    return 0;
}
