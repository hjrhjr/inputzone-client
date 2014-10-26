#include <Iz_EmLib.h>
#include <libcxx/iosfwd>
#include <libc/bits/alltypes.h>
#include <libcxx/string>


void Convert(std::string id,FILE* input, uint64_t inputSize,std::string directoryPath,std::string fileBasename, std::string inputExtension ){
    FILE *output;
    char buf[1024*100];
    uint64_t output_offset = 0;

    std::string outputFullPath = directoryPath+fileBasename+"2"+inputExtension;

    iz_print(outputFullPath);
    if((output=fopen(outputFullPath.c_str(), "wb")) == NULL) {
        iz_error("Cannot create the output file.");
        return;
    }
    //setvbuf ( output, NULL , _IOFBF , 32541536 );

    uint64_t lastTruncate = 0;
    unsigned int n = 0;
    do {
        n = fread(buf, sizeof(char), sizeof(buf), input);
        fwrite(buf, sizeof(char), n, output);
        output_offset+=n;
        lastTruncate+=n;
/*        if(lastTruncate>100000000)
        {
            input = iz_truncateInput(lastTruncate, true);
            lastTruncate=0;
        }*/

        iz_updateProgress((uint8_t)((float)output_offset/(float)inputSize*100));
    }while(n!=0);

    iz_print("fileoutputSize="+std::to_string(output_offset));
    iz_release(output);

}


extern "C"{
void initWorker(char *data, int size){
    EM_ASM_ARGS({
        Module.print(Pointer_stringify($0));
    },"initing");
    iz_init(data, size, Convert,emscripten_worker_respond_provisionally,emscripten_worker_respond );
}

void workerReady(char *data, int size){
/*    std::string s = data;
    iz_print(s);
    iz_print(std::to_string(size));
    iz_print("Ready!!!");*/
    //emscripten_worker_respond("lol",3);
}
}
