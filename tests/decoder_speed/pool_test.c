#include "../../patches/decoder_speed/pool.h"
#include <assert.h>
#include <string.h>

int main(void) {
    ds_pool p={0}; uint8_t bytes[128]; ds_failure failed={0};
    assert(ds_pool_add(&p,2,bytes,64));
    assert(ds_pool_add(&p,3,bytes+64,32));
    const ds_request batch[]={{2,64},{3,32}};
    assert(ds_pool_preflight(&p,batch,2,&failed));
    assert(!ds_pool_alloc(&p,63)); // Wrong-size new never consumes a reservation.
    assert(ds_pool_alloc(&p,64)==bytes);
    assert(ds_pool_alloc(&p,32)==bytes+64);
    assert(!ds_pool_alloc(&p,8));
    assert(!ds_pool_reset(&p));
    assert(ds_pool_free(&p,bytes)); assert(!ds_pool_free(&p,bytes));
    assert(ds_pool_free(&p,bytes+64)); assert(ds_pool_reset(&p));
    const ds_request bad[]={{2,64},{3,33}};
    assert(!ds_pool_preflight(&p,bad,2,&failed));
    assert(failed.tag==3 && failed.size==33 && !p.queued && !p.slots[0].pending);
    assert(ds_pool_preflight(&p,batch,2,&failed));
    assert(ds_pool_alloc(&p,64)==bytes); assert(ds_pool_alloc(&p,32)==bytes+64);
    assert(ds_pool_free(&p,bytes)); assert(ds_pool_free(&p,bytes+64));
    assert(ds_pool_reset(&p));
    assert(ds_pool_add(&p,0,bytes+96,32));
    assert(ds_pool_alloc(&p,4)==bytes+96); assert(ds_pool_free(&p,bytes+96));
    assert(p.highwater==96 && !p.live);
    return 0;
}
