/* Public CRC API checked against independent reflected bit-by-bit arithmetic. */
#include "bplib_crc.h"
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/** Compute CRC16 or CRC32C bit by bit as an independent reference for the table-driven
 * implementation. */
static uint64_t reference(const unsigned char *data, size_t size, unsigned width)
{
    uint32_t crc=width==16 ? UINT32_C(0xffff) : UINT32_C(0xffffffff);
    uint32_t polynomial=width==16 ? UINT32_C(0x8408) : UINT32_C(0x82f63b78);
    for (size_t i=0;i<size;++i)
    {
        crc^=data[i];
        for (unsigned bit=0;bit<8;++bit)
            crc=(crc>>1)^((crc&1) ? polynomial : 0);
    }
    return width==16 ? (crc^UINT32_C(0xffff)) : (crc^UINT32_C(0xffffffff));
}
static unsigned checks;
/** Check both public CRC variants against the bitwise reference for one buffer. */
static void compare(const unsigned char *data,size_t size)
{
    assert(BPLib_CRC_Calculate(data,size,BPLib_CRC_Type_CRC16)==reference(data,size,16));
    assert(BPLib_CRC_Calculate(data,size,BPLib_CRC_Type_CRC32C)==reference(data,size,32));
    checks+=2;
}
/** Exercise known vectors, varied lengths, unaligned input, and repeat initialization. */
int main(void)
{
    static const unsigned char text[]="123456789";
    BPLib_CRC_Init();
    assert(BPLib_CRC_Calculate(text,9,BPLib_CRC_Type_CRC16)==UINT64_C(0x906e));
    assert(BPLib_CRC_Calculate(text,9,BPLib_CRC_Type_CRC32C)==UINT64_C(0xe3069283));
    compare(NULL,0);
    assert(BPLib_CRC_Calculate(NULL,0,BPLib_CRC_Type_None)==0);
    assert(BPLib_CRC_Calculate(text,9,UINT8_MAX)==0);
    unsigned char buffer[4098]; uint32_t seed=UINT32_C(0x71d0beef);
    for (size_t i=0;i<sizeof(buffer);++i)
    { seed=seed*UINT32_C(1664525)+UINT32_C(1013904223); buffer[i]=(unsigned char)(seed>>24); }
    const size_t boundaries[]={0,1,2,3,7,8,9,15,16,17,31,32,33,63,64,65,
        127,128,129,255,256,257,511,512,513,1023,1024,1025,2031,2032,2033,4096,4097};
    for (size_t i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);++i)
    { compare(buffer,boundaries[i]); compare(buffer+1,boundaries[i]); }
    for (unsigned byte=0;byte<256;++byte)
    { buffer[0]=(unsigned char)byte; compare(buffer,1); }
    for (unsigned trial=0;trial<32;++trial)
    { seed=seed*UINT32_C(1664525)+UINT32_C(1013904223); compare(buffer+1,seed%4098); }
    BPLib_CRC_Init(); compare(text,9);
    printf("PASS known CRC vectors and %u differential checks, empty/unaligned inputs, and reinitialization\n",checks);
    return 0;
}
