/*
PRISM_BAKE_BATCH.C

The irradiance volume's batches and their readbacks (prism_bake_batch.h).
*/

#include "prism_bake_batch.h"

#include <string.h>

void prism_bake_batches_start(struct prism_bake_batches *batches, uint32_t total, uint32_t size)
{
	memset(batches, 0, sizeof(*batches));
	batches->total = total;
	batches->size = size ? size : 1u;
}

void prism_bake_batches_skip(struct prism_bake_batches *batches, uint32_t done)
{
	if (batches->batched > batches->done || done > batches->total)
		return;
	batches->done = done;
	batches->batched = done;
}

uint32_t prism_bake_batches_begin(struct prism_bake_batches *batches)
{
	uint32_t count;

	if (batches->batched > batches->done || batches->done >= batches->total)
		return 0;
	count = batches->total - batches->done;
	if (count > batches->size)
		count = batches->size;
	batches->batched = batches->done + count;
	batches->number++;
	if (!batches->number)
		batches->number = 1;
	batches->asked = 0;
	return count;
}

uint32_t prism_bake_batches_counting(struct prism_bake_batches const *batches)
{
	return batches->batched > batches->done && !batches->asked ? batches->number : 0u;
}

uint32_t prism_bake_batches_counted(struct prism_bake_batches *batches, uint32_t number, uint32_t running)
{
	if (running || !number || number != batches->number || batches->asked || batches->batched <= batches->done)
		return 0;
	batches->asked = 1;
	return batches->batched - batches->done;
}

int64_t prism_bake_batches_finished(struct prism_bake_batches *batches, uint32_t number, uint32_t count)
{
	uint32_t first = batches->done;

	if (!number || number != batches->number || !batches->asked || count != batches->batched - batches->done)
		return -1;
	batches->done = batches->batched;
	batches->asked = 0;
	return (int64_t)first;
}
