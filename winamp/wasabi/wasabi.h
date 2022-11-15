#ifndef WASABI_H_
#define WASABI_H_

#ifdef __cplusplus
extern "C" {
#endif

void Wasabi_Init();
void Wasabi_Quit();
void *Wasabi_Malloc(size_t size_in_bytes);
void Wasabi_Free(void *memory_block);

#ifdef __cplusplus
}
#endif
#endif /* WASABI_H_ */
