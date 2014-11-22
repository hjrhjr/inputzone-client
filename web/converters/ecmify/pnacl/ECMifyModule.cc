#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "IzInstance.h"
#include "FileConverter.h"

/*
"ECMify - Decoder for Error Code Modeler format v1.0\n"
    "Copyright (C) 2002 Neill Corlett\n\n"
 */


/* Data types */
typedef uint8_t ecc_uint8;
typedef uint16_t ecc_uint16;
typedef uint32_t ecc_uint32;

/* LUTs used for computing ECC/EDC */
static ecc_uint8 ecc_f_lut[256];
static ecc_uint8 ecc_b_lut[256];
static ecc_uint32 edc_lut[256];

/* Init routine */
static void eccedc_init(void) {
    ecc_uint32 i, j, edc;
    for(i = 0; i < 256; i++) {
        j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        ecc_f_lut[i] = j;
        ecc_b_lut[i ^ j] = i;
        edc = i;
        for(j = 0; j < 8; j++) edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
        edc_lut[i] = edc;
    }
}

/***************************************************************************/
/*
** Compute EDC for a block
*/
ecc_uint32 edc_computeblock(
        ecc_uint32  edc,
        const ecc_uint8  *src,
        ecc_uint16  size
) {
    while(size--) edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
    return edc;
}

/***************************************************************************/
/*
** Compute ECC for a block (can do either P or Q)
*/
static int ecc_computeblock(
        ecc_uint8 *src,
        ecc_uint32 major_count,
        ecc_uint32 minor_count,
        ecc_uint32 major_mult,
        ecc_uint32 minor_inc,
        ecc_uint8 *dest
) {
    ecc_uint32 size = major_count * minor_count;
    ecc_uint32 major, minor;
    for(major = 0; major < major_count; major++) {
        ecc_uint32 index = (major >> 1) * major_mult + (major & 1);
        ecc_uint8 ecc_a = 0;
        ecc_uint8 ecc_b = 0;
        for(minor = 0; minor < minor_count; minor++) {
            ecc_uint8 temp = src[index];
            index += minor_inc;
            if(index >= size) index -= size;
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }
        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        if(dest[major              ] != (ecc_a        )) return 0;
        if(dest[major + major_count] != (ecc_a ^ ecc_b)) return 0;
    }
    return 1;
}

/*
** Generate ECC P and Q codes for a block
*/
static int ecc_generate(
        ecc_uint8 *sector,
        int        zeroaddress,
        ecc_uint8 *dest
) {
    int r;
    ecc_uint8 address[4], i;
    /* Save the address and zero it out */
    if(zeroaddress) for(i = 0; i < 4; i++) {
            address[i] = sector[12 + i];
            sector[12 + i] = 0;
        }
    /* Compute ECC P code */
    if(!(ecc_computeblock(sector + 0xC, 86, 24,  2, 86, dest + 0x81C - 0x81C))) {
        if(zeroaddress) for(i = 0; i < 4; i++) sector[12 + i] = address[i];
        return 0;
    }
    /* Compute ECC Q code */
    r = ecc_computeblock(sector + 0xC, 52, 43, 86, 88, dest + 0x8C8 - 0x81C);
    /* Restore the address */
    if(zeroaddress) for(i = 0; i < 4; i++) sector[12 + i] = address[i];
    return r;
}

/***************************************************************************/

/*
** sector types:
** 00 - literal bytes
** 01 - 2352 mode 1         predict sync, mode, reserved, edc, ecc
** 02 - 2336 mode 2 form 1  predict redundant flags, edc, ecc
** 03 - 2336 mode 2 form 2  predict redundant flags, edc
*/

int check_type(unsigned char *sector, int canbetype1) {
    int canbetype2 = 1;
    int canbetype3 = 1;
    ecc_uint32 myedc;
    /* Check for mode 1 */
    if(canbetype1) {
        if(
                (sector[0x00] != 0x00) ||
                        (sector[0x01] != 0xFF) ||
                        (sector[0x02] != 0xFF) ||
                        (sector[0x03] != 0xFF) ||
                        (sector[0x04] != 0xFF) ||
                        (sector[0x05] != 0xFF) ||
                        (sector[0x06] != 0xFF) ||
                        (sector[0x07] != 0xFF) ||
                        (sector[0x08] != 0xFF) ||
                        (sector[0x09] != 0xFF) ||
                        (sector[0x0A] != 0xFF) ||
                        (sector[0x0B] != 0x00) ||
                        (sector[0x0F] != 0x01) ||
                        (sector[0x814] != 0x00) ||
                        (sector[0x815] != 0x00) ||
                        (sector[0x816] != 0x00) ||
                        (sector[0x817] != 0x00) ||
                        (sector[0x818] != 0x00) ||
                        (sector[0x819] != 0x00) ||
                        (sector[0x81A] != 0x00) ||
                        (sector[0x81B] != 0x00)
                ) {
            canbetype1 = 0;
        }
    }
    /* Check for mode 2 */
    if(
            (sector[0x0] != sector[0x4]) ||
                    (sector[0x1] != sector[0x5]) ||
                    (sector[0x2] != sector[0x6]) ||
                    (sector[0x3] != sector[0x7])
            ) {
        canbetype2 = 0;
        canbetype3 = 0;
        if(!canbetype1) return 0;
    }

    /* Check EDC */
    myedc = edc_computeblock(0, sector, 0x808);
    if(canbetype2) if(
            (sector[0x808] != ((myedc >>  0) & 0xFF)) ||
                    (sector[0x809] != ((myedc >>  8) & 0xFF)) ||
                    (sector[0x80A] != ((myedc >> 16) & 0xFF)) ||
                    (sector[0x80B] != ((myedc >> 24) & 0xFF))
            ) {
        canbetype2 = 0;
    }
    myedc = edc_computeblock(myedc, sector + 0x808, 8);
    if(canbetype1) if(
            (sector[0x810] != ((myedc >>  0) & 0xFF)) ||
                    (sector[0x811] != ((myedc >>  8) & 0xFF)) ||
                    (sector[0x812] != ((myedc >> 16) & 0xFF)) ||
                    (sector[0x813] != ((myedc >> 24) & 0xFF))
            ) {
        canbetype1 = 0;
    }
    myedc = edc_computeblock(myedc, sector + 0x810, 0x10C);
    if(canbetype3) if(
            (sector[0x91C] != ((myedc >>  0) & 0xFF)) ||
                    (sector[0x91D] != ((myedc >>  8) & 0xFF)) ||
                    (sector[0x91E] != ((myedc >> 16) & 0xFF)) ||
                    (sector[0x91F] != ((myedc >> 24) & 0xFF))
            ) {
        canbetype3 = 0;
    }
    /* Check ECC */
    if(canbetype1) { if(!(ecc_generate(sector       , 0, sector + 0x81C))) { canbetype1 = 0; } }
    if(canbetype2) { if(!(ecc_generate(sector - 0x10, 1, sector + 0x80C))) { canbetype2 = 0; } }
    if(canbetype1) return 1;
    if(canbetype2) return 2;
    if(canbetype3) return 3;
    return 0;
}

/***************************************************************************/
/*
** Encode a type/count combo
*/
void write_type_count(
        FILE *out,
        unsigned type,
        unsigned count
) {
    count--;
    fputc(((count >= 32) << 7) | ((count & 31) << 2) | type, out);
    count >>= 5;
    while(count) {
        fputc(((count >= 128) << 7) | (count & 127), out);
        count >>= 7;
    }
}

/***************************************************************************/

unsigned long mycounter_analyze;
unsigned long mycounter_encode;
unsigned long mycounter_total;

void resetcounter(unsigned long total) {
    mycounter_analyze = 0;
    mycounter_encode = 0;
    mycounter_total = total;
}



/***************************************************************************/

unsigned char inputqueue[1048576 + 4];


class ECMifyConverter : public FileConverter {
public:
    ECMifyConverter( IzInstanceBase *instance,const pp::Var& var_message) : FileConverter(instance, var_message){
    };
    static int64_t estimateOutputSize(int64_t inputSize){
        return 700*1024*1024; //approximate size of a psx iso
    }

    static std::string baseParameters(uint64_t taskId, std::string inputFullPath, uint64_t inputSize, std::string outputDirectoryPath, std::string baseName, std::string inputExtension){
        return "\\\""+inputFullPath+"\\\" \\\""+outputDirectoryPath+baseName+inputExtension+".ecm\\\"";
    }
private:

    int main(int argc, char **argv) {
        FILE *fin, *fout;
        char *infilename;
        char *outfilename;
        /*
        ** Initialize the ECC/EDC tables
        */
        eccedc_init();
        /*
        ** Check command line
        */
        if((argc != 2) && (argc != 3)) {
            Error("usage: "+std::string(argv[0])+" cdimagefile [ecmfile]");
            return 1;
        }
        infilename = argv[1];
        /*
        ** Figure out what the output filename should be
        */
        if(argc == 3) {
            outfilename = argv[2];
        } else {
            outfilename = (char*)malloc(strlen(infilename) + 5);
            if(!outfilename) abort();
            sprintf(outfilename, "%s.ecm", infilename);
        }
        Console("Encoding"+std::string(infilename)+" to "+std::string(outfilename)+".");
        /*
        ** Open both files
        */
        fin = fopen(infilename, "rb");
        if(!fin) {
            perror(infilename);
            Error("Can't open input file!");
            return 1;
        }
        fout = fopen(outfilename, "wb");
        if(!fout) {
            perror(outfilename);
            fclose(fin);
            Error("Can't open output file!");
            return 1;
        }
        /*
        ** Encode
        */
        setvbuf ( fout, NULL , _IOFBF , 32541536 );
        ecmify(fin, fout);
        /*
        ** Close everything
        */
        fclose(fout);
        fclose(fin);
        return 0;
    }

   /* virtual int32_t Convert(double taskId, FILE* input, uint64_t inputSize, std::string directoryPath, std::string baseName, std::string inputExtension) {
        FILE *output;


        eccedc_init();

        std::string outputFullPath = directoryPath+baseName+inputExtension+".ecm";
        if((output=fopen(outputFullPath.c_str(), "wb")) == NULL) {
            Error("Cannot create the output file.");
            return -1;
        }
        setvbuf ( output, NULL , _IOFBF , 32541536 );

        return ecmify(input, output);
    };*/

    unsigned in_flush(
            unsigned edc,
            unsigned type,
            unsigned count,
            FILE *in,
            FILE *out
    ) {
        unsigned char buf[2352];
        write_type_count(out, type, count);
        if(!type) {
            while(count) {
                unsigned b = count;
                if(b > 2352) b = 2352;
                fread(buf, 1, b, in);
                edc = edc_computeblock(edc, buf, b);
                fwrite(buf, 1, b, out);
                count -= b;
                setcounter_encode(ftell(in));
            }
            return edc;
        }
        while(count--) {
            switch(type) {
                case 1:
                    fread(buf, 1, 2352, in);
                    edc = edc_computeblock(edc, buf, 2352);
                    fwrite(buf + 0x00C, 1, 0x003, out);
                    fwrite(buf + 0x010, 1, 0x800, out);
                    setcounter_encode(ftell(in));
                    break;
                case 2:
                    fread(buf, 1, 2336, in);
                    edc = edc_computeblock(edc, buf, 2336);
                    fwrite(buf + 0x004, 1, 0x804, out);
                    setcounter_encode(ftell(in));
                    break;
                case 3:
                    fread(buf, 1, 2336, in);
                    edc = edc_computeblock(edc, buf, 2336);
                    fwrite(buf + 0x004, 1, 0x918, out);
                    setcounter_encode(ftell(in));
                    break;
            }
        }
        return edc;
    }


    void setcounter_analyze(unsigned long n) {
        if((n >> 20) != (mycounter_analyze >> 20)) {
            unsigned long a = (n+64)/128;
            unsigned long e = (mycounter_encode+64)/128;
            unsigned long d = (mycounter_total+64)/128;
            UpdatePreProgress((100*a) / d);
            UpdateProgress((100*e) / d);
        }
        mycounter_analyze = n;
    }

    void setcounter_encode(unsigned long n) {
        if((n >> 20) != (mycounter_encode >> 20)) {
            unsigned long a = (mycounter_analyze+64)/128;
            unsigned long e = (n+64)/128;
            unsigned long d = (mycounter_total+64)/128;
            if(!d) d = 1;
            UpdatePreProgress((100*a) / d);
            UpdateProgress((100*e) / d);
        }
        mycounter_encode = n;
    }



    int ecmify(FILE *in, FILE *out) {
        unsigned inedc = 0;
        int curtype = -1;
        int curtypecount = 0;
        int curtype_in_start = 0;
        int detecttype;
        int incheckpos = 0;
        long inbufferpos = 0;
        long intotallength;
        int inqueuestart = 0;
        size_t dataavail = 0;
        int typetally[4];
        fseek(in, 0, SEEK_END);
        intotallength = ftell(in);
        resetcounter(intotallength);
        typetally[0] = 0;
        typetally[1] = 0;
        typetally[2] = 0;
        typetally[3] = 0;
        /* Magic identifier */
        fputc('E', out);
        fputc('C', out);
        fputc('M', out);
        fputc(0x00, out);
        for(;;) {
            if((dataavail < 2352) && (dataavail < (intotallength - inbufferpos))) {
                long willread = intotallength - inbufferpos;
                if(willread > ((sizeof(inputqueue) - 4) - dataavail)) willread = (sizeof(inputqueue) - 4) - dataavail;
                if(inqueuestart) {
                    memmove(inputqueue + 4, inputqueue + 4 + inqueuestart, dataavail);
                    inqueuestart = 0;
                }
                if(willread) {
                    setcounter_analyze(inbufferpos);
                    fseek(in, inbufferpos, SEEK_SET);
                    fread(inputqueue + 4 + dataavail, 1, willread, in);
                    inbufferpos += willread;
                    dataavail += willread;
                }
            }
            if(dataavail <= 0) break;
            if(dataavail < 2336) {
                detecttype = 0;
            } else {
                detecttype = check_type(inputqueue + 4 + inqueuestart, dataavail >= 2352);
            }
            if(detecttype != curtype) {
                if(curtypecount) {
                    fseek(in, curtype_in_start, SEEK_SET);
                    typetally[curtype] += curtypecount;
                    inedc = in_flush(inedc, curtype, curtypecount, in, out);
                }
                curtype = detecttype;
                curtype_in_start = incheckpos;
                curtypecount = 1;
            } else {
                curtypecount++;
            }
            switch(curtype) {
                case 0: incheckpos +=    1; inqueuestart +=    1; dataavail -=    1; break;
                case 1: incheckpos += 2352; inqueuestart += 2352; dataavail -= 2352; break;
                case 2: incheckpos += 2336; inqueuestart += 2336; dataavail -= 2336; break;
                case 3: incheckpos += 2336; inqueuestart += 2336; dataavail -= 2336; break;
            }
            if(isCancelling_){
                fclose(out);
                UpdateTaskStatus(CANCELED);
                return -1;
            }
        }
        if(curtypecount) {
            fseek(in, curtype_in_start, SEEK_SET);
            typetally[curtype] += curtypecount;
            inedc = in_flush(inedc, curtype, curtypecount, in, out);
        }
        /* End-of-records indicator */
        write_type_count(out, 0, 0);
        /* Input file EDC */
        fputc((inedc >>  0) & 0xFF, out);
        fputc((inedc >>  8) & 0xFF, out);
        fputc((inedc >> 16) & 0xFF, out);
        fputc((inedc >> 24) & 0xFF, out);
        /* Show report */
        fprintf(stderr, "Literal bytes........... %10d\n", typetally[0]);
        fprintf(stderr, "Mode 1 sectors.......... %10d\n", typetally[1]);
        fprintf(stderr, "Mode 2 form 1 sectors... %10d\n", typetally[2]);
        fprintf(stderr, "Mode 2 form 2 sectors... %10d\n", typetally[3]);
        fprintf(stderr, "Encoded %ld bytes -> %ld bytes\n", intotallength, ftell(out));
        fprintf(stderr, "Done.\n");
        return PP_OK;
    }


};


class ECMifyModule : public pp::Module {
public:
    ECMifyModule() : pp::Module() {}
    virtual ~ECMifyModule() {}

    virtual pp::Instance* CreateInstance(PP_Instance instance) {
        return new IzInstance<ECMifyConverter>(instance);
    }
};

namespace pp {
    Module* CreateModule() {
        return new ECMifyModule();
    }
}



