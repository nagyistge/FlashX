#ifndef __IO_REQUEST_H__
#define __IO_REQUEST_H__

/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SAFSlib.
 *
 * SAFSlib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SAFSlib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SAFSlib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/uio.h>
#include <sys/time.h>
#include <limits.h>

#include "common.h"
#include "concurrency.h"

class thread_safe_page;
class io_interface;

class io_buf
{
	union {
		thread_safe_page *p;
		void *buf;
	} u;
	unsigned int size: 31;
	unsigned int is_page: 1;
public:
	io_buf() {
		u.buf = NULL;
		size = 0;
		is_page = 0;
	}

	void init(void *p, int size, bool is_page) {
		u.buf = p;
		if (is_page)
			assert(size == PAGE_SIZE);
		this->size = size;
		this->is_page = is_page;
	}

	void init(thread_safe_page *p);
	void init(void *buf, int size) {
		u.buf = buf;
		this->size = size;
		is_page = 0;
	}

	void *get_buf() const;

	int get_size() const {
		return size;
	}

	thread_safe_page *get_page() const {
		assert(is_page);
		return u.p;
	}
};

class io_request;

class io_req_extension
{
	static atomic_unsigned_integer num_creates;
	io_request *orig;
	void *priv;
	void *user_data;

	int num_bufs: 16;
	// Is the request part of a request?
	int partial: 1;
	int vec_capacity: 15;

	/* 
	 * This is to protect the object from being removed
	 * while others are still using it.
	 */
	atomic_number<short> refcnt;

	atomic_number<ssize_t> completed_size;

	io_buf *vec_pointer;
	io_buf embedded_vecs[NUM_EMBEDDED_IOVECS];
	io_request *next;

	struct timeval issue_time;

public:
	io_req_extension() {
		num_creates.inc(1);
		vec_pointer = embedded_vecs;
		vec_capacity = NUM_EMBEDDED_IOVECS;
		init();
	}

	~io_req_extension() {
		if (vec_pointer != embedded_vecs)
			delete [] vec_pointer;
	}

	bool is_valid() const {
		// Valid extension must have vec_pointer points to embedded_vecs
		// or an array.
		return vec_pointer != NULL;
	}

	void init() {
		this->orig = NULL;
		this->priv = NULL;
		this->user_data = NULL;
		this->num_bufs = 0;
		this->partial = 0;
		memset(vec_pointer, 0, vec_capacity * sizeof(io_buf));
		next = NULL;
		memset(&issue_time, 0, sizeof(issue_time));
		refcnt = atomic_number<short>();
		completed_size = atomic_number<ssize_t>();
	}

	void init(const io_req_extension &ext) {
		this->orig = ext.orig;
		this->priv = ext.priv;
		this->user_data = ext.user_data;
		this->num_bufs = ext.num_bufs;
		this->partial = ext.partial;
		assert(this->vec_capacity >= ext.vec_capacity);
		assert(this->refcnt.get() == 0);
		assert(this->completed_size.get() == 0);
		memcpy(vec_pointer, ext.vec_pointer, num_bufs * sizeof(*vec_pointer));
		assert(this->next == NULL);
		memset(&issue_time, 0, sizeof(issue_time));
	}

	io_request *get_orig() const {
		return orig;
	}

	void set_orig(io_request *orig) {
		this->orig = orig;
	}

	void *get_priv() const {
		return priv;
	}

	void set_priv(void *priv) {
		this->priv = priv;
	}

	void *get_user_data() const {
		return user_data;
	}

	void set_user_data(void *user_data) {
		this->user_data = user_data;
	}

	void set_partial(bool partial) {
		this->partial = partial;
	}

	bool is_partial() const {
		return partial;
	}

	io_request *get_next() const {
		return next;
	}

	void set_next(io_request *next) {
		this->next = next;
	}

	int inc_ref() {
		return refcnt.inc(1);
	}

	int dec_ref() {
		return refcnt.dec(1);
	}

	int get_ref() const {
		return refcnt.get();
	}

	ssize_t inc_completed_size(ssize_t size) {
		return completed_size.inc(size);
	}

	ssize_t get_completed_size() const {
		return completed_size.get();
	}

	void set_timestamp() {
		gettimeofday(&issue_time, NULL);
	}

	struct timeval get_timestamp() {
		return issue_time;
	}

	void add_io_buf(const io_buf &buf);
	void add_buf(char *buf, int size, bool is_page);
	void add_buf_front(char *buf, int size, bool is_page);
	int get_num_bufs() const {
		return num_bufs;
	}

	const io_buf &get_buf(int idx) const {
		return vec_pointer[idx];
	}

	int get_size() const {
		ssize_t size = 0;
		for (int i = 0; i < num_bufs; i++)
			size += vec_pointer[i].get_size();
		return size;
	}
};

const int MAX_INLINE_SIZE=128;

class user_compute
{
public:
	virtual int serialize(char *buf, int size) const = 0;
	virtual int get_serialized_size() const = 0;
};

typedef int file_id_t;
const int INVALID_FILE_ID = -1;

class data_loc_t
{
	file_id_t file_id;
	off_t off;
public:
	data_loc_t() {
		file_id = -1;
		off = -1;
	}

	data_loc_t(file_id_t file_id, off_t off) {
		this->file_id = file_id;
		this->off = off;
	}

	file_id_t get_file_id() const {
		return file_id;
	}

	off_t get_offset() const {
		return off;
	}
};

const data_loc_t INVALID_DATA_LOC;

/**
 * This class contains the request info.
 * If it's in an extended form, it is created by the global cache
 * and is used within a NUMA machine.
 * It is decided when the request is created whether or not it is
 * an extended request. It can't be changed afterwards.
 */
class io_request
{
	enum {
		BASIC_REQ,
		EXT_REQ,
		USER_COMPUTE,
	};

	int file_id;
	static const off_t MAX_FILE_SIZE = LONG_MAX;
	off_t offset: 48;
	static const size_t MAX_BUF_SIZE = (1L << 32) - 1;
	unsigned long buf_size_low: 16;
	long user_data_addr: 48;
	unsigned long buf_size_high: 16;

	// These two flags decide how the payload is interpreted, so they are
	// initialized when the object is created and can't be changed manually.
	// The only case that payload_type is changed is when
	// another request object is copied to this request object.
	// What needs to be guaranteed is that the pointer in the payload always
	// points to the object of the right type.
	// However, data_inline is never changed. It is usually false. It is
	// used when to interpret the io requests in a message.
	unsigned int payload_type: 2;
	unsigned int data_inline: 1;

	unsigned int access_method: 1;
	// Is this synchronous IO?
	unsigned int sync: 1;
	unsigned int high_prio: 1;
	unsigned int low_latency: 1;
	unsigned int discarded: 1;
	static const int MAX_NODE_ID = (1 << 8) - 1;
	unsigned int node_id: 8;
	// Linux uses 48 bit for addresses.
	// When the request is completed, the IO instance will be notified of
	// the completion. The IO is usually the issuer IO, but it can be other
	// IOs.
	unsigned long io_addr: 48;

	union {
		/**
		 * These two can be used by users.
		 */
		void *buf_addr;
		user_compute *compute;

		/**
		 * This field should only be used by the library itself.
		 */
		io_req_extension *ext;

		/**
		 * This field is used to help address the memory following the request.
		 * It is used when a user wants to embed user data in the request.
		 */
		char buf[0];
	} payload;

	void use_default_flags() {
		sync = 0;
		high_prio = 1;
		low_latency = 0;
		discarded = 0;
	}

	void copy_flags(const io_request &req) {
		this->sync = req.sync;
		this->high_prio = req.high_prio;
		this->low_latency = req.low_latency;
	}

	void set_int_buf_size(size_t size) {
		assert(size <= MAX_BUF_SIZE);
		buf_size_high = size >> 16;
		buf_size_low = size & 0xFFFFL;
	}

	size_t get_int_buf_size() const {
		return (buf_size_high << 16) + buf_size_low;
	}

public:
	// By default, a request is initialized as a flush request.
	io_request(bool sync = false) {
		memset(this, 0, sizeof(*this));
		payload_type = BASIC_REQ;
		use_default_flags();
		this->sync = sync;
	}

	io_request(char *buf, const data_loc_t &loc, ssize_t size,
			int access_method, io_interface *io, int node_id, bool sync = false) {
		payload_type = BASIC_REQ;
		data_inline = 0;
		user_data_addr = 0;
		init(buf, loc, size, access_method, io, node_id);
		use_default_flags();
		this->sync = sync;
	}

	io_request(io_req_extension *ext, const data_loc_t &loc, int access_method,
			io_interface *io, int node_id, bool sync = false) {
		payload_type = EXT_REQ;
		data_inline = 0;
		payload.ext = ext;
		user_data_addr =0;
		init(NULL, loc, 0, access_method, io, node_id);
		use_default_flags();
		this->sync = sync;
	}

	io_request(user_compute *compute, const data_loc_t &loc, ssize_t size,
			int access_method, io_interface *io, int node_id,
			bool sync = false) {
		payload_type = USER_COMPUTE;
		data_inline = 0;
		user_data_addr = 0;
		init(NULL, loc, size, access_method, io, node_id);
		payload.compute = compute;
		use_default_flags();
		this->sync = sync;
	}

	void init(const io_request &req) {
		data_loc_t loc(req.get_file_id(), req.get_offset());
		assert(!data_inline);
		if (req.payload_type == USER_COMPUTE
				|| this->payload_type == USER_COMPUTE) {
			assert(req.payload_type == USER_COMPUTE
					&& this->payload_type == USER_COMPUTE);
			// TODO
		}
		else if (!req.is_extended_req()) {
			this->init(req.get_buf(), loc, req.get_size(),
					req.get_access_method(), req.get_io(), req.get_node_id());
		}
		// Both requests have extensions.
		else if (this->is_extended_req()) {
			this->init(NULL, loc, 0, req.get_access_method(),
					req.get_io(), req.get_node_id());
			this->get_extension()->init(*req.get_extension());
		}
		else {
			// The last case is that this request doesn't have extension,
			// but the given request has extension. This request can't keep
			// all information in the given request.
			assert(!this->is_extended_req() && req.is_extended_req());
		}
		copy_flags(req);
		this->user_data_addr = req.user_data_addr;
	}

	void init() {
		data_inline = 0;
		if (is_extended_req()) {
			io_req_extension *ext = get_extension();
			assert(ext);
			ext->init();
		}
		else {
			payload_type = BASIC_REQ;
			payload.buf_addr = NULL;
		}
		file_id = 0;
		offset = 0;
		high_prio = 0;
		sync = 0;
		node_id = 0;
		io_addr = 0;
		access_method = 0;
		set_int_buf_size(0);
		user_data_addr = 0;
	}

	void init(char *buf, const data_loc_t &loc, ssize_t size,
			int access_method, io_interface *io, int node_id);
	void init(const data_loc_t &loc, int access_method, io_interface *io,
			int node_id) {
		init(NULL, loc, 0, access_method, io, node_id);
	}

	io_req_extension *get_extension() const {
		assert(is_extended_req() && payload.ext);
		if (data_inline)
			return (io_req_extension *) payload.buf;
		else
			return payload.ext;
	}

	file_id_t get_file_id() const;

	/**
	 * Test whether the request is a flush request.
	 * The difference of a sync'd request and a flush request is that
	 * a flush request isn't a valid request for accessing data.
	 */
	bool is_flush() const {
		return sync && high_prio && (payload.buf_addr == NULL);
	}

	bool is_sync() const {
		return sync;
	}

	bool is_extended_req() const {
		return payload_type == EXT_REQ;
	}

	off_t get_offset() const {
		return offset;
	}

	void set_data_loc(const data_loc_t &loc) {
		this->file_id = loc.get_file_id();
		this->offset = loc.get_offset();
	}

	int get_access_method() const {
		return access_method & 0x1;
	}

	void set_io(io_interface *io) {
		this->io_addr = (long) io;
	}

	io_interface *get_io() const {
		return (io_interface *) (long) io_addr;
	}

	int get_node_id() const {
		return node_id;
	}

	void set_node_id(int node_id) {
		assert(node_id <= MAX_NODE_ID);
		this->node_id = node_id;
	}

	bool is_discarded() const {
		return (discarded & 0x1) == 1;
	}

	void set_discarded(bool discarded) {
		this->discarded = discarded;
	}

	bool is_high_prio() const {
		return (high_prio & 0x1) == 1;
	}

	void set_high_prio(bool high_prio) {
		this->high_prio = high_prio;
	}

	bool is_low_latency() const {
		return (low_latency & 0x1) == 1;
	}

	void set_low_latency(bool low_latency) {
		this->low_latency = low_latency;
	}

	/**
	 * The requested data is inside a page on the disk.
	 */
	bool within_1page() const {
		return get_offset() + get_size() <= ROUND_PAGE(get_offset()) + PAGE_SIZE;
	}

	bool inside_RAID_block() const {
		int RAID_block_size = params.get_RAID_block_size() * PAGE_SIZE;
		return ROUND(this->get_offset(), RAID_block_size)
			== ROUND(this->get_offset() + this->get_size() - 1, RAID_block_size);
	}

	io_request *get_orig() const {
		return get_extension()->get_orig();
	}

	void set_orig(io_request *orig) {
		get_extension()->set_orig(orig);
	}

	void *get_user_data() const {
		return (void *) (long) user_data_addr;
	}

	void set_user_data(void *data) {
		user_data_addr = (long) data;
	}

	void *get_priv() const {
		return get_extension()->get_priv();
	}

	void set_priv(void *priv) {
		get_extension()->set_priv(priv);
	}

	bool is_empty() const {
		return get_extension()->get_num_bufs() == 0;
	}

	bool is_valid() const {
		return get_offset() != -1;
	}

	ssize_t get_size() const {
		if (!is_extended_req()) {
			return get_int_buf_size();
		}
		else {
			return get_extension()->get_size();
		}
	}

	/**
	 * By default, we get the first buffer. This makes sense
	 * for a single buffer request.
	 */
	char *get_buf(int idx = 0) const {
		if (!is_extended_req()) {
			return (char *) payload.buf_addr;
		}
		else {
			return (char *) get_extension()->get_buf(idx).get_buf();
		}
	}

	thread_safe_page *get_page(int idx) const {
		return get_extension()->get_buf(idx).get_page();
	}

	void add_buf(char *buf, int size) {
		get_extension()->add_buf(buf, size, 0);
	}

	void add_page(thread_safe_page *p) {
		get_extension()->add_buf((char *) p, PAGE_SIZE, 1);
	}

	void add_io_buf(const io_buf &buf) {
		get_extension()->add_io_buf(buf);
	}

	void add_buf_front(char *buf, int size) {
		get_extension()->add_buf_front(buf, size, 0);
	}

	void add_page_front(thread_safe_page *p) {
		get_extension()->add_buf_front((char *) p, PAGE_SIZE, 1);
	}

	int get_num_bufs() const {
		if (is_extended_req())
			return get_extension()->get_num_bufs();
		else
			return 1;
	}

	int get_buf_size(int idx) const {
		if (!is_extended_req()) {
			assert(idx == 0);
			return get_int_buf_size();
		}
		else
			return get_extension()->get_buf(idx).get_size();
	}

	const io_buf &get_io_buf(int idx) const {
		return get_extension()->get_buf(idx);
	}

	const struct iovec get(int idx) const {
		struct iovec ret;
		io_req_extension *ext = get_extension();
		ret.iov_base = ext->get_buf(idx).get_buf();
		ret.iov_len = ext->get_buf(idx).get_size();
		return ret;
	}

	const int get_vec(struct iovec *vec, int num) const {
		num = min(get_num_bufs(), num);
		for (int i = 0; i < num; i++) {
			io_req_extension *ext = get_extension();
			vec[i].iov_base = ext->get_buf(i).get_buf();
			vec[i].iov_len = ext->get_buf(i).get_size();
		}
		return num;
	}

	io_request *get_next_req() const {
		return get_extension()->get_next();
	}

	void set_next_req(io_request *next) {
		get_extension()->set_next(next);
	}

	int inc_complete_count() {
		return get_extension()->inc_ref();
	}

	int dec_complete_count() {
		return get_extension()->dec_ref();
	}

	void wait4unref() {
		while (get_extension()->get_ref() > 0) {}
	}

	/**
	 * Maintain the completed size in this request.
	 * If the request is complete, return true;
	 */
	bool complete_size(ssize_t completed) {
		ssize_t res = get_extension()->inc_completed_size(completed);;
		ssize_t size = get_size();
		assert(res <= size);
		return res == size;
	}

	bool is_complete() const {
		return get_extension()->get_completed_size() == get_size();
	}

	void set_partial(bool partial) {
		get_extension()->set_partial(partial);
	}

	bool is_partial() const {
		return get_extension()->is_partial();
	}

	bool is_data_inline() const {
		return data_inline == 1;
	}

	void set_timestamp() {
		get_extension()->set_timestamp();
	}

	struct timeval get_timestamp() {
		return get_extension()->get_timestamp();
	}

	/**
	 * Extract a request from the input request.
	 * The extract request is within the range [off, off + size).
	 */
	void extract(off_t off, int size, io_request &extracted) const {
		off_t req_off;
		char *req_buf;
		ssize_t req_size;
		assert(get_num_bufs() == 1);
		// We have to make sure the extracted range has overlap with
		// the input request.
		// Either the beginning of the extracted range inside the input request
		bool check = (off >= this->get_offset() && off < this->get_offset()
				+ this->get_size())
			// or the end of the extracted range inside the input request
			|| (off + size >= this->get_offset()
					&& off + size < this->get_offset() + this->get_size())
			// or the input request is inside the extracted range.
			|| (off <= this->get_offset()
					&& off + size >= this->get_offset() + this->get_size());
		if (!check)
			fprintf(stderr, "req %lx, size: %lx, page off: %lx\n",
					this->get_offset(), this->get_size(), off);
		assert(check);
		// this is the first page in the request.
		if (off <= this->get_offset()) {
			req_off = this->get_offset();
			req_buf = this->get_buf();
		}
		else {
			req_off = off;
			/* 
			 * We can't be sure if the request buffer is aligned
			 * with the page size.
			 */
			req_buf = this->get_buf() + (off - this->get_offset());
		}
		req_size = min(off + size - req_off,
				this->get_offset() + this->get_size() - req_off);
		data_loc_t loc(this->get_file_id(), req_off);
		extracted.init(req_buf, loc, req_size, this->get_access_method(),
				this->get_io(), this->get_node_id());
	}

	/**
	 * We need to serialize an io request to a buffer so it can be sent to
	 * another thread.
	 * @accept_inline: indicates whether the IO request can inline data
	 * to the buffer.
	 */
	int serialize(char *buf, int size, bool accept_inline) {
		int serialized_size;
		if (is_data_inline()) {
			assert(accept_inline);
			serialized_size = get_serialized_size();
			assert(serialized_size <= size);
			memcpy(buf, this, serialized_size);
		}
		else if (payload_type == EXT_REQ) {
			// We never serialize the io request extension to the message.
			serialized_size = sizeof(io_request);
			assert(serialized_size <= size);
			memcpy(buf, this, sizeof(*this));
			payload.ext = NULL;
			payload_type = BASIC_REQ;
		}
		else if (payload_type == BASIC_REQ) {
			// We only serialize the data buffer to the message for write
			// requests. The size of the data buffer has to be small.
			if (get_size() > MAX_INLINE_SIZE || access_method == READ
					|| !accept_inline) {
				serialized_size = sizeof(io_request);
				assert(serialized_size <= size);
				memcpy(buf, this, sizeof(*this));
			}
			else {
				serialized_size = sizeof(io_request) - sizeof(this->payload)
					+ get_size();
				assert(serialized_size <= size);
				memcpy(buf, this, sizeof(*this));
				io_request *p = (io_request *) buf;
				p->data_inline = 1;
				memcpy(p->payload.buf, (char *) this->payload.buf_addr,
						this->get_size());
			}
		}
		else {
			assert(accept_inline);
			// The user compute object is always serialized to the message.
			user_compute *compute = this->payload.compute;
			serialized_size = sizeof(io_request) - sizeof(this->payload)
				+ compute->get_serialized_size();
			assert(serialized_size <= size && sizeof(io_request)
					<= (unsigned) size);
			memcpy(buf, this, sizeof(*this));
			io_request *p = (io_request *) buf;
			p->data_inline = 1;
			compute->serialize((char *) p->payload.buf,
					size - (sizeof(io_request) - sizeof(this->payload)));
		}
		return serialized_size;
	}

	/**
	 * This method returns the size of an IO request after it is serialized.
	 */
	int get_serialized_size() const {
		if (payload_type == EXT_REQ)
			return sizeof(io_request);
		else if (payload_type == BASIC_REQ && (get_size() > MAX_INLINE_SIZE
					|| access_method == READ))
			return sizeof(io_request);
		else if (payload_type == BASIC_REQ)
			return sizeof(io_request) - sizeof(this->payload) + get_size();
		else if (!is_data_inline()) {
			user_compute *compute = this->payload.compute;
			return sizeof(io_request) - sizeof(this->payload)
				+ compute->get_serialized_size();
		}
		else {
			user_compute *compute = (user_compute *) this->payload.buf;
			return sizeof(io_request) - sizeof(this->payload)
				+ compute->get_serialized_size();
		}
	}

	/**
	 * This method deserialize an request from the buffer.
	 * If the request data is inline in the buffer, instead of allocating
	 * memory for the extra objects of the IO request, the extra objects
	 * will stay in the buffer and the created request will point to the buffer.
	 */
	static void deserialize(io_request &req, char *buf, int size) {
		assert((unsigned) size >= sizeof(io_request));
		io_request *p = (io_request *) buf;
		memcpy(&req, buf, sizeof(req));
		if (req.is_data_inline()) {
			assert(req.payload_type != EXT_REQ);
			switch(req.payload_type) {
				case BASIC_REQ:
					req.payload.buf_addr = p->payload.buf;
					break;
				case USER_COMPUTE:
					req.payload.compute = (user_compute *) p->payload.buf;
					break;
				default:
					assert(0);
			}
			req.data_inline = 0;
		}
	}

	static io_request *deserialize(char *buf, int size) {
		assert((unsigned) size >= sizeof(io_request));
		io_request *ret = (io_request *) buf;
		assert(ret->get_serialized_size() <= size);
		return ret;
	}
};

typedef void (*req_process_func_t)(io_interface *io, io_request *reqs[], int num);
/**
 * Perform the same function to the requests with the same IO instance.
 * It turns out it's a common function when delivering requests to
 * the upper layer.
 */
void process_reqs_on_io(io_request *reqs[], int num, req_process_func_t func);

#endif
