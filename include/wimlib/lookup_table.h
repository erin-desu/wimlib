#ifndef _WIMLIB_LOOKUP_TABLE_H
#define _WIMLIB_LOOKUP_TABLE_H

#include "wimlib/assert.h"
#include "wimlib/dentry.h"
#include "wimlib/list.h"
#include "wimlib/sha1.h"
#include "wimlib/types.h"
#include "wimlib/wim.h"

#define LOOKUP_FLAG_ADS_OK		0x00000001
#define LOOKUP_FLAG_DIRECTORY_OK	0x00000002


/* The lookup table of a WIM file maps SHA1 message digests to streams of data.
 * Here, the in-memory structure is implemented as a hash table.
 *
 * Given a SHA1 message digest, the mapped-to stream is specified by an offset
 * in the WIM, an uncompressed and compressed size, and resource flags (see
 * 'struct resource_entry').  But, we associate additional information, such as
 * a reference count, with each stream, so the actual mapping is from SHA1
 * message digests to 'struct wim_lookup_table_entry's, each of which contains
 * an embedded 'struct resource_entry'.
 *
 * Note: Everything will break horribly if there is a SHA1 collision.
 */
struct wim_lookup_table {
	struct hlist_head *array;
	u64 num_entries;
	u64 capacity;
	struct list_head *unhashed_streams;
};

#ifdef WITH_NTFS_3G

struct _ntfs_volume;

struct ntfs_location {
	tchar *path;
	utf16lechar *stream_name;
	u16 stream_name_nchars;
	struct _ntfs_volume *ntfs_vol;
	bool is_reparse_point;
};
#endif

/* An enumerated type that identifies where the stream corresponding to this
 * lookup table entry is actually located.
 *
 * If we open a WIM and read its lookup table, the location is set to
 * RESOURCE_IN_WIM since all the streams will initially be located in the WIM.
 * However, to handle situations such as image capture and image mount, we allow
 * the actual location of the stream to be somewhere else, such as an external
 * file.
 */
enum resource_location {
	/* The lookup table entry does not yet correspond to a stream; this is a
	 * temporary state only.  */
	RESOURCE_NONEXISTENT = 0,

	/* The stream is located in a resource in a WIM file identified by the
	 * `struct wim_resource_spec' pointed to by @rspec.  @offset_in_res
	 * identifies the offset at which this particular stream begins in the
	 * uncompressed data of the resource; this is normally 0, but in general
	 * a WIM resource may be "packed" and potentially contain multiple
	 * streams.  */
	RESOURCE_IN_WIM,

	/* The stream is located in the external file named by @file_on_disk.
	 * On Windows, @file_on_disk may actually specify a named data stream
	 * (file path, then colon, then name of the stream).  */
	RESOURCE_IN_FILE_ON_DISK,

	/* The stream is directly attached in the in-memory buffer pointed to by
	 * @attached_buffer.  */
	RESOURCE_IN_ATTACHED_BUFFER,

#ifdef WITH_FUSE
	/* The stream is located in the external file named by
	 * @staging_file_name, located in the staging directory for a read-write
	 * mount.  */
	RESOURCE_IN_STAGING_FILE,
#endif

#ifdef WITH_NTFS_3G
	/* The stream is located in an NTFS volume.  It is identified by volume,
	 * filename, data stream name, and by whether it is a reparse point or
	 * not.  @ntfs_loc points to a structure containing this information.
	 * */
	RESOURCE_IN_NTFS_VOLUME,
#endif

#ifdef __WIN32__
	/* Windows only: the stream is located in the external file named by
	 * @file_on_disk, but the file is encrypted and must be read using the
	 * appropriate Windows API.  */
	RESOURCE_WIN32_ENCRYPTED,
#endif

};

/* Specification for a stream, which may be the contents of a file (unnamed data
 * stream), a named data stream, reparse point data, or a WIM metadata resource.
 *
 * One instance of this structure is created for each entry in the WIM's lookup
 * table, hence the name of the struct.  Each of these entries contains the SHA1
 * message digest of a stream and the location of the stream data in the WIM
 * file (size, location, flags).  The in-memory lookup table is a map from SHA1
 * message digests to stream locations.  */
struct wim_lookup_table_entry {

	/* List node for a hash bucket of the lookup table.  */
	struct hlist_node hash_list;

	/* Uncompressed size of this stream.  */
	u64 size;

	/* Stream flags (WIM_RESHDR_FLAG_*).  */
	u32 flags : 8;

	/* One of the `enum resource_location' values documented above.  */
	u32 resource_location : 4;

	/* 1 if this stream has not had a SHA1 message digest calculated for it
	 * yet.  */
	u32 unhashed : 1;

	/* Temoorary fields used when writing streams; set as documented for
	 * prepare_stream_list_for_write().  */
	u32 unique_size : 1;
	u32 will_be_in_output_wim : 1;

	/* Set to 1 when a metadata entry has its checksum changed; in such
	 * cases the hash cannot be used to verify the data if the metadata
	 * resource is read again.  (This could be avoided if we used separate
	 * fields for input/output checksum, but most stream entries wouldn't
	 * need this.)  */
	u32 dont_check_metadata_hash : 1;

	union {
		/* (On-disk field) SHA1 message digest of the stream referenced
		 * by this lookup table entry.  */
		u8  hash[SHA1_HASH_SIZE];

		/* First 4 or 8 bytes of the SHA1 message digest, used for
		 * inserting the entry into the hash table.  Since the SHA1
		 * message digest can be considered random, we don't really need
		 * the full 20 byte hash just to insert the entry in a hash
		 * table.  */
		size_t hash_short;

		/* Unhashed entries only (unhashed == 1): these variables make
		 * it possible to find the pointer to this 'struct
		 * wim_lookup_table_entry' contained in either 'struct
		 * wim_ads_entry' or 'struct wim_inode'.  There can be at most 1
		 * such pointer, as we can only join duplicate streams after
		 * they have been hashed.  */
		struct {
			struct wim_inode *back_inode;
			u32 back_stream_id;
		};
	};

	/* Number of times this lookup table entry is referenced by dentries in
	 * the WIM.  When a WIM's lookup table is read, this field is
	 * initialized from a corresponding entry; while it should be correct,
	 * in general it may not be.  wim_recalculate_refcnts() recalculates the
	 * reference counts for all streams and is run before doing any
	 * deletions.  */
	u32 refcnt;

	/* When a WIM file is written, this is set to the number of references
	 * (by dentries) to this stream in the output WIM file.
	 *
	 * During extraction, this is set to the number of times the stream must
	 * be extracted.
	 *
	 * During image export, this is set to the number of references of this
	 * stream that originated from the source WIM.  */
	u32 out_refcnt;

#ifdef WITH_FUSE
	/* Number of times this stream has been opened; used only during
	 * mounting.  */
	u16 num_opened_fds;
#endif

	/* Specification of where this stream is actually located.  Which member
	 * is valid is determined by the @resource_location field.  */
	union {
		struct {
			struct wim_resource_spec *rspec;
			u64 offset_in_res;
		};
		tchar *file_on_disk;
		void *attached_buffer;
	#ifdef WITH_FUSE
		tchar *staging_file_name;
	#endif
	#ifdef WITH_NTFS_3G
		struct ntfs_location *ntfs_loc;
	#endif
	};

	/* Links together streams that share the same underlying WIM resource.
	 * The head is the `stream_list' member of `struct wim_resource_spec'.
	 */
	struct list_head rspec_node;

	/* This field is used during the hardlink and symlink image extraction
	 * modes.   In these modes, all identical files are linked together, and
	 * @extracted_file will be set to the filename of the first extracted
	 * file containing this stream.  */
	tchar *extracted_file;

	/* Temporary fields  */
	union {
		/* Fields used temporarily during WIM file writing.  */
		struct {
			union {
				/* List node used for stream size table.  */
				struct hlist_node hash_list_2;

				/* Metadata for the underlying packed resource
				 * in the WIM being written (only valid if
				 * WIM_RESHDR_FLAG_PACKED_STREAMS set in
				 * out_reshdr.flags).  */
				struct {
					u64 out_res_offset_in_wim;
					u64 out_res_size_in_wim;
				};
			};

			/* Links streams being written to the WIM.  */
			struct list_head write_streams_list;

			/* Metadata for this stream in the WIM being written.
			 */
			struct wim_reshdr out_reshdr;
		};

		/* Used temporarily during extraction  */
		union {
			/* Dentries to extract that reference this stream.
			 * out_refcnt tracks the number of slots filled.  */
			struct wim_dentry *inline_lte_dentries[7];
			struct {
				struct wim_dentry **lte_dentries;
				size_t alloc_lte_dentries;
			};
		};

		/* Actual reference count to this stream (only used while
		 * verifying an image).  */
		u32 real_refcnt;
	};

	/* Temporary list fields.  */
	union {
		/* Links streams for writing lookup table.  */
		struct list_head lookup_table_list;

		/* Links streams being extracted.  */
		struct list_head extraction_list;

		/* Links streams being exported.  */
		struct list_head export_stream_list;
	};

	/* Links streams that are still unhashed after being been added to a
	 * WIM.  */
	struct list_head unhashed_list;
};

static inline bool
lte_is_partial(const struct wim_lookup_table_entry * lte)
{
	return lte->resource_location == RESOURCE_IN_WIM &&
	       lte->size != lte->rspec->uncompressed_size;
}

static inline bool
lte_filename_valid(const struct wim_lookup_table_entry *lte)
{
	return     lte->resource_location == RESOURCE_IN_FILE_ON_DISK
	#ifdef __WIN32__
		|| lte->resource_location == RESOURCE_WIN32_ENCRYPTED
	#endif
	#ifdef WITH_FUSE
		|| lte->resource_location == RESOURCE_IN_STAGING_FILE
	#endif
		;
}

extern struct wim_lookup_table *
new_lookup_table(size_t capacity) _malloc_attribute;

extern int
read_wim_lookup_table(WIMStruct *wim);

extern int
write_wim_lookup_table_from_stream_list(struct list_head *stream_list,
					struct filedes *out_fd,
					u16 part_number,
					struct wim_reshdr *out_reshdr,
					int write_resource_flags);

extern void
free_lookup_table(struct wim_lookup_table *table);

extern void
lookup_table_insert(struct wim_lookup_table *table, struct wim_lookup_table_entry *lte);

/* Unlinks a lookup table entry from the table; does not free it.  */
static inline void
lookup_table_unlink(struct wim_lookup_table *table, struct wim_lookup_table_entry *lte)
{
	wimlib_assert(!lte->unhashed);
	hlist_del(&lte->hash_list);
	wimlib_assert(table->num_entries != 0);
	table->num_entries--;
}

extern struct wim_lookup_table_entry *
new_lookup_table_entry(void) _malloc_attribute;

extern struct wim_lookup_table_entry *
clone_lookup_table_entry(const struct wim_lookup_table_entry *lte)
			_malloc_attribute;

extern void
print_lookup_table_entry(const struct wim_lookup_table_entry *lte, FILE *out);

extern void
free_lookup_table_entry(struct wim_lookup_table_entry *lte);

extern void
lte_to_wimlib_resource_entry(const struct wim_lookup_table_entry *lte,
			     struct wimlib_resource_entry *wentry);

extern int
for_lookup_table_entry(struct wim_lookup_table *table,
		       int (*visitor)(struct wim_lookup_table_entry *, void *),
		       void *arg);

extern int
sort_stream_list(struct list_head *stream_list,
		 size_t list_head_offset,
		 int (*compar)(const void *, const void*));

extern int
sort_stream_list_by_sequential_order(struct list_head *stream_list,
				     size_t list_head_offset);

extern int
for_lookup_table_entry_pos_sorted(struct wim_lookup_table *table,
				  int (*visitor)(struct wim_lookup_table_entry *,
						 void *),
				  void *arg);

extern struct wim_lookup_table_entry *
lookup_resource(const struct wim_lookup_table *table, const u8 hash[]);

extern int
wim_pathname_to_stream(WIMStruct *wim, const tchar *path,
		       int lookup_flags,
		       struct wim_dentry **dentry_ret,
		       struct wim_lookup_table_entry **lte_ret,
		       u16 *stream_idx_ret);

extern void
lte_decrement_refcnt(struct wim_lookup_table_entry *lte,
		     struct wim_lookup_table *table);
#ifdef WITH_FUSE
extern void
lte_decrement_num_opened_fds(struct wim_lookup_table_entry *lte);
#endif

extern int
lte_zero_out_refcnt(struct wim_lookup_table_entry *lte, void *ignore);

extern int
lte_zero_real_refcnt(struct wim_lookup_table_entry *lte, void *ignore);

extern int
lte_free_extracted_file(struct wim_lookup_table_entry *lte, void *ignore);

static inline void
lte_bind_wim_resource_spec(struct wim_lookup_table_entry *lte,
			   struct wim_resource_spec *rspec)
{
	lte->resource_location = RESOURCE_IN_WIM;
	lte->rspec = rspec;
	list_add_tail(&lte->rspec_node, &rspec->stream_list);
}

static inline void
lte_unbind_wim_resource_spec(struct wim_lookup_table_entry *lte)
{
	list_del(&lte->rspec_node);
	lte->resource_location = RESOURCE_NONEXISTENT;
}

extern int
inode_resolve_ltes(struct wim_inode *inode, struct wim_lookup_table *table,
		   bool force);

extern int
resource_not_found_error(const struct wim_inode *inode, const u8 *hash);

extern void
inode_unresolve_ltes(struct wim_inode *inode);

static inline struct wim_lookup_table_entry *
inode_stream_lte_resolved(const struct wim_inode *inode, unsigned stream_idx)
{
	wimlib_assert(inode->i_resolved);
	wimlib_assert(stream_idx <= inode->i_num_ads);
	if (stream_idx == 0)
		return inode->i_lte;
	else
		return inode->i_ads_entries[stream_idx - 1].lte;
}

static inline struct wim_lookup_table_entry *
inode_stream_lte_unresolved(const struct wim_inode *inode, unsigned stream_idx,
			    const struct wim_lookup_table *table)
{
	wimlib_assert(!inode->i_resolved);
	wimlib_assert(stream_idx <= inode->i_num_ads);
	if (!table)
		return NULL;
	if (stream_idx == 0)
		return lookup_resource(table, inode->i_hash);
	else
		return lookup_resource(table,
					 inode->i_ads_entries[
						stream_idx - 1].hash);
}

extern struct wim_lookup_table_entry *
inode_stream_lte(const struct wim_inode *inode, unsigned stream_idx,
		 const struct wim_lookup_table *table);

static inline const u8 *
inode_stream_hash_unresolved(const struct wim_inode *inode, unsigned stream_idx)
{
	wimlib_assert(!inode->i_resolved);
	wimlib_assert(stream_idx <= inode->i_num_ads);
	if (stream_idx == 0)
		return inode->i_hash;
	else
		return inode->i_ads_entries[stream_idx - 1].hash;
}


static inline const u8 *
inode_stream_hash_resolved(const struct wim_inode *inode, unsigned stream_idx)
{
	struct wim_lookup_table_entry *lte;
	lte = inode_stream_lte_resolved(inode, stream_idx);
	if (lte)
		return lte->hash;
	else
		return zero_hash;
}

/*
 * Returns the hash for stream @stream_idx of the inode, where stream_idx = 0
 * means the default un-named file stream, and stream_idx >= 1 corresponds to an
 * alternate data stream.
 *
 * This works for both resolved and un-resolved dentries.
 */
static inline const u8 *
inode_stream_hash(const struct wim_inode *inode, unsigned stream_idx)
{
	if (inode->i_resolved)
		return inode_stream_hash_resolved(inode, stream_idx);
	else
		return inode_stream_hash_unresolved(inode, stream_idx);
}

static inline u16
inode_stream_name_nbytes(const struct wim_inode *inode, unsigned stream_idx)
{
	wimlib_assert(stream_idx <= inode->i_num_ads);
	if (stream_idx == 0)
		return 0;
	else
		return inode->i_ads_entries[stream_idx - 1].stream_name_nbytes;
}

extern struct wim_lookup_table_entry *
inode_unnamed_stream_resolved(const struct wim_inode *inode, u16 *stream_idx_ret);

extern struct wim_lookup_table_entry *
inode_unnamed_lte_resolved(const struct wim_inode *inode);

extern struct wim_lookup_table_entry *
inode_unnamed_lte_unresolved(const struct wim_inode *inode,
			     const struct wim_lookup_table *table);

extern struct wim_lookup_table_entry *
inode_unnamed_lte(const struct wim_inode *inode, const struct wim_lookup_table *table);

extern const u8 *
inode_unnamed_stream_hash(const struct wim_inode *inode);

static inline void
lookup_table_insert_unhashed(struct wim_lookup_table *table,
			     struct wim_lookup_table_entry *lte,
			     struct wim_inode *back_inode,
			     u32 back_stream_id)
{
	lte->unhashed = 1;
	lte->back_inode = back_inode;
	lte->back_stream_id = back_stream_id;
	list_add_tail(&lte->unhashed_list, table->unhashed_streams);
}

extern int
hash_unhashed_stream(struct wim_lookup_table_entry *lte,
		     struct wim_lookup_table *lookup_table,
		     struct wim_lookup_table_entry **lte_ret);

extern struct wim_lookup_table_entry **
retrieve_lte_pointer(struct wim_lookup_table_entry *lte);

#endif /* _WIMLIB_LOOKUP_TABLE_H */
