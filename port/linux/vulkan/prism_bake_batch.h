/*
PRISM_BAKE_BATCH.H

The batches of the irradiance volume's probes (xgpu_vulkan_bake.c): a
batch is begun on the GPU, its count of running probes is read back each
frame until it is 0, then its results are read back once and written.

The readbacks are several frames behind the frame that asks for them, and
while the game draws several are in flight at once: two or three counts of
the same batch can come back 0. Only the first asks for the results; the
others, and any count or result of a batch that is no longer running, are
let go. Each readback carries the batch it was made for (its number).

Host code with no engine dependency, written as C89.
*/

#ifndef PRISM_BAKE_BATCH_H
#define PRISM_BAKE_BATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct prism_bake_batches
{
	uint32_t total; /* the items */
	uint32_t size; /* the most a batch holds */
	uint32_t done; /* the items whose results are written */
	uint32_t batched; /* done and the running batch's */
	uint32_t number; /* the running batch's (1 the first) */
	int asked; /* its results asked for */
};

void prism_bake_batches_start(struct prism_bake_batches *batches, uint32_t total, uint32_t size);

/* items already done (the file had them): done moved on, with no batch
running */
void prism_bake_batches_skip(struct prism_bake_batches *batches, uint32_t done);

/* the next batch begun: its count (0 while one runs, or none is left) */
uint32_t prism_bake_batches_begin(struct prism_bake_batches *batches);

/* the batch whose running count to read back this frame (its number), or
0: none runs, or its results are asked for already */
uint32_t prism_bake_batches_counting(struct prism_bake_batches const *batches);

/* a count read back of the batch numbered: the batch's count if its
results are to be asked for now (it runs, its count is 0, they were not
asked for), else 0 */
uint32_t prism_bake_batches_counted(struct prism_bake_batches *batches, uint32_t number, uint32_t running);

/* the results of the batch numbered read back, count of them: the first
item they are of, done moved past them; -1 if they are not the running
batch's asked results (let go) */
int64_t prism_bake_batches_finished(struct prism_bake_batches *batches, uint32_t number, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
