/*
XGPU_MESH_STORE.H

PRISM's mesh store (display.prism_models; docs/mods.md, "Meshes"):
replacement meshes a mod ships as .pmesh files, read by the host straight
from their files into the GPU's memory, never into the guest's. The guest
goes on loading and drawing the stock model, which stays the fallback and
what the game simulates with; it learns only an id for each model that has
a replacement (the stream's MESH_STORE_MAP), sends each drawn instance's
node matrices (MESH_INSTANCE) and names the part each draw is of
(MESH_DRAW).

The .pmesh format (version 1, little-endian; tools/pmesh.py writes and
checks it, the art pipeline's exporter makes it):

	header, 64 bytes: magic 'PMSH' (XGPU_PMESH_MAGIC), version (uint16, 1),
	flags (uint16, 0), the file's size, the sections' count and the table's
	offset, the model's node count and checksum (mod_meshes.c's), the bone
	count (the nodes' first, then helper bones), the level of detail count,
	the bounds' centre (3 floats) and radius, 3 reserved words
	a section table: {kind, offset, size, CRC-32} each, inside the file
	SKEL: a bone each, 112 bytes: name hash, parent (int16, -1 none), flags
	  (uint16: 1 one of the model's nodes, 2 a helper), the model's node
	  (int16, -1 a helper), 2 bytes, the name's offset in the names that
	  follow, its bind pose (3 rows of 4, model space) and the inverse; then
	  the names, each ending in its NUL. Bone i < node count is the model's
	  node i, skinned by that node's matrix; a helper by its nearest node
	  ancestor's (until rigs drive helpers)
	LODS: a level each, 48 bytes: the pixels it is drawn from (the size the
	  game chooses its levels by; the first level the most detailed), its
	  pieces (first and count), its vertices (first and count) and indices
	  (first and count), its bounds' centre and radius, a reserved word
	SUBM: a piece each, 24 bytes: region, permutation, shader (the model's
	  indices), flags (bytes), a material's hash (0 none), its vertices
	  (first and count) and indices (first and count)
	VERT: a vertex each, 48 bytes: position f32x3; normal and tangent,
	  octahedral snorm16x2 each; uv1 f16x2; uv0 f32x2; joints uint16x4
	  (the mesh's bones); weights unorm8x4 (summing to 255); the tangent's
	  sign (int8: negative or not) and 3 bytes
	INDX: uint32 each, a piece's relative to its first vertex, triangle
	  lists (each a triangle as the stock part's front face winds it)
	META: anything (the exporter's notes); skipped

The store's portable core (xgpu_mesh_store.c), with no device of its own: a
backend (Vulkan's, xgpu_vulkan_meshes.c; the tests' fake) makes buffers and
copies into them. The core:
- checks each file's header, skeleton, levels and pieces at the map's
  record, synchronously (a few KB a file), against the guest's query: the
  node count and checksum, the parts the model can draw;
- reads a mesh's vertices and indices when it is first drawn, on a worker
  thread, into a staging buffer the backend maps: their CRCs, every joint,
  weight and index, and (the stock layout's limit) every uv0 inside the
  model's base map scale checked before any is drawn;
- copies them into the mesh's buffers at the frame's start, and from then on
  answers a draw with the level its size chooses;
- keeps the meshes within a budget (auto: a fifth of the device's free
  memory, 128 MB to 1 GB; or the setting): a mesh not drawn in the last
  XGPU_MESH_STORE_KEEP_FRAMES frames is let go of for another, the least
  recently drawn first; one that does not fit is drawn stock.

The render thread calls every function; none may be called from two threads
at once.
*/

#ifndef __HALO_LINUX_XGPU_MESH_STORE_H
#define __HALO_LINUX_XGPU_MESH_STORE_H

#include <stdint.h>

#define XGPU_PMESH_MAGIC 0x48534d50u /* "PMSH" */
#define XGPU_PMESH_VERSION 1
#define XGPU_PMESH_HEADER_BYTES 64u
#define XGPU_PMESH_SECTION_BYTES 16u
#define XGPU_PMESH_BONE_BYTES 112u
#define XGPU_PMESH_LOD_BYTES 48u
#define XGPU_PMESH_PIECE_BYTES 24u
#define XGPU_PMESH_VERTEX_BYTES 48u
#define XGPU_PMESH_KIND(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define XGPU_PMESH_SKEL XGPU_PMESH_KIND('S', 'K', 'E', 'L')
#define XGPU_PMESH_LODS XGPU_PMESH_KIND('L', 'O', 'D', 'S')
#define XGPU_PMESH_SUBM XGPU_PMESH_KIND('S', 'U', 'B', 'M')
#define XGPU_PMESH_VERT XGPU_PMESH_KIND('V', 'E', 'R', 'T')
#define XGPU_PMESH_INDX XGPU_PMESH_KIND('I', 'N', 'D', 'X')
#define XGPU_PMESH_META XGPU_PMESH_KIND('M', 'E', 'T', 'A')
#define XGPU_PMESH_BONES_MAXIMUM 1024u
#define XGPU_PMESH_LODS_MAXIMUM 8u
#define XGPU_PMESH_PIECES_MAXIMUM 4096u
#define XGPU_PMESH_SECTIONS_MAXIMUM 32u
/* a sanity bound on a mesh's vertices and indices, not a design limit */
#define XGPU_PMESH_ELEMENTS_MAXIMUM (1u << 28)

/* meshes drawn in this many frames are never let go of */
#define XGPU_MESH_STORE_KEEP_FRAMES 10u
/* a mesh that did not fit is tried again after this many frames */
#define XGPU_MESH_STORE_RETRY_FRAMES 120u
/* the most entries a map has */
#define XGPU_MESH_STORE_ENTRIES_MAXIMUM 4096u
#define XGPU_MESH_STORE_ID(generation, index) ((((generation) & 0xfffu) << 20) | ((index) + 1u))

/* a buffer the backend makes */
enum
{
	XGPU_MESH_STORE_BUFFER_REST,
	XGPU_MESH_STORE_BUFFER_INDICES,
	XGPU_MESH_STORE_BUFFER_STAGING,
};

struct xgpu_mesh_store_backend
{
	void *context;
	/* NULL if it cannot be made; a staging buffer's mapped address in
	*mapped; bytes: its memory's */
	void *(*buffer_create)(void *context, int kind, uint64_t size, void **mapped, uint64_t *bytes);
	/* after the work recorded so far */
	void (*buffer_destroy)(void *context, void *buffer);
	/* in the work recorded now, before the frame's draws */
	void (*copy)(void *context, void *from, uint64_t from_offset, void *to, uint64_t to_offset, uint64_t size);
	/* the device's memory: its budget and what is used of it, or its size
	and 0; 0 if not known */
	int (*memory)(void *context, uint64_t *budget, uint64_t *usage);
	void (*log)(void *context, int warning, const char *text);
};

struct xgpu_mesh_store_settings
{
	/* a worker thread (0: none, the tests' xgpu_mesh_store_work) */
	uint32_t workers;
	/* the auto budget's least and most */
	uint64_t budget_floor, budget_ceiling;
};

/* a file the map's record asks about (the stream's query, its path and
table found) */
struct xgpu_mesh_store_query
{
	const char *path;
	uint64_t file_size;
	uint32_t stamp_seconds;
	uint32_t node_count, node_crc;
	/* table_count entries of 4 bytes: region, permutation, shader, 0 */
	const uint8_t *table;
	uint32_t table_count;
	float base_map_scale[2];
};

/* the store's answer (the stream's) */
struct xgpu_mesh_store_answer
{
	uint32_t id;
	uint32_t status;
	float render_radius;
	uint32_t bones, lods;
};

/* a piece of a mesh (SUBM) */
struct xgpu_mesh_store_piece
{
	uint8_t region, permutation, shader, flags;
	uint32_t material;
	uint32_t vertex_first, vertex_count, index_first, index_count;
};

/* what a draw of a mesh that is in the GPU's memory reads: its buffers, the
level chosen (its vertices, which are skinned together, and pieces), its
bones and each bone's node (a helper's nearest node ancestor's), the uv
scale the stock layout's texture coordinates are of (1 over the base map
scale), and whether a region and permutation (region * 32 + permutation)
has any piece in any level (else its parts are drawn stock) */
struct xgpu_mesh_store_view
{
	void *rest, *indices;
	uint32_t lod;
	uint32_t vertex_first, vertex_count;
	const struct xgpu_mesh_store_piece *pieces;
	uint32_t piece_count;
	uint32_t bone_count;
	const uint16_t *bone_nodes;
	uint32_t node_count;
	float inverse_scale[2];
	const uint8_t *present;
};

struct xgpu_mesh_store_statistics
{
	uint32_t entries, resident, failed, reading, refused_budget;
	uint64_t resident_bytes, budget_bytes, read_bytes;
	uint32_t evictions;
	uint64_t uses, stale_uses, stock_uses;
	/* seconds from the map's record to the last mesh made resident */
	double last_resident_seconds;
};

struct xgpu_mesh_store;

struct xgpu_mesh_store *xgpu_mesh_store_create(const struct xgpu_mesh_store_backend *backend,
	const struct xgpu_mesh_store_settings *settings);
/* the worker stopped, every buffer destroyed */
void xgpu_mesh_store_destroy(struct xgpu_mesh_store *store);

/* A map's files (MESH_STORE_MAP): a new generation (nonzero), what the old
one had let go of (its reads finished and dropped, its buffers destroyed),
each query's file checked, its answer written (XGPU_STREAM_MESH_STORE_*
statuses). count 0 closes the store. budget_megabytes: 0 auto. */
void xgpu_mesh_store_map(struct xgpu_mesh_store *store, uint32_t generation, uint32_t budget_megabytes,
	uint32_t count, const struct xgpu_mesh_store_query *queries, struct xgpu_mesh_store_answer *answers);
/* an id drawn this frame at a size (lod_pixels): 1 and the view where its
mesh is in the GPU's memory, else 0 (drawn stock; its reading begun) */
int xgpu_mesh_store_use(struct xgpu_mesh_store *store, uint32_t id, float lod_pixels,
	struct xgpu_mesh_store_view *view);
/* the frame's start: meshes read copied into their buffers, the budget kept */
void xgpu_mesh_store_frame(struct xgpu_mesh_store *store);
void xgpu_mesh_store_statistics(const struct xgpu_mesh_store *store, struct xgpu_mesh_store_statistics *statistics);
/* (tests, with no worker) one queued read done on the calling thread: 0 if
none was queued */
int xgpu_mesh_store_work(struct xgpu_mesh_store *store);

/* the standard CRC-32 (zlib's) of bytes, continued from crc */
uint32_t xgpu_mesh_store_crc32(uint32_t crc, const void *data, uint64_t size);

#endif
