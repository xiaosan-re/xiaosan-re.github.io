#include <stdint.h>
#ifndef QUOTE_NAME
#define QUOTE_NAME quote_demo
#endif
__attribute__((annotate("omvll.business-vm")))
uint32_t QUOTE_NAME(uint32_t user, uint32_t quantity,
                    uint32_t tier, uint32_t token) {
    uint32_t expected = (user * 17u + 9u) ^ 0x13579bdfu;
    if (token != expected) return 0;
    uint32_t gross = quantity * (100u + tier * 5u);
    uint32_t discount = quantity >= 10u ? gross / 10u : 0u;
    return gross - discount;
}
