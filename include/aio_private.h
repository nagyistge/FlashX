#ifndef __AIO_PRIVATE_H__
#define __AIO_PRIVATE_H__

#include <deque>
#include <tr1/unordered_map>

#include "wpaio.h"
#include "read_private.h"
#include "thread.h"
#include "container.h"
#include "messaging.h"
#include "slab_allocator.h"

void aio_callback(io_context_t, struct iocb*, void *, long, long);

struct thread_callback_s;
class aio_complete_thread;

class aio_complete_queue
{
	blocking_FIFO_queue<thread_callback_s *> queue;
public:
	aio_complete_queue(int node_id): queue(node_id, "aio_completes",
			// The max size of the queue can be unlimited.
			// The number of completed requests is limited by the number of
			// requests issued by the user.
			AIO_DEPTH_PER_FILE, INT_MAX) {
	}

	blocking_FIFO_queue<thread_callback_s *> *get_queue() {
		return &queue;
	}

	int process(int max_num, bool blocking);
};

class aio_complete_sender: public simple_sender<thread_callback_s *>
{
public:
	aio_complete_sender(int node_id,
			aio_complete_queue *queue): simple_sender<thread_callback_s *>(
			node_id, queue->get_queue(), AIO_DEPTH_PER_FILE) {
	}
};

class async_io;
class callback_allocator;

struct thread_callback_s
{
	struct io_callback_s cb;
	async_io *aio;
	callback *aio_callback;
	callback_allocator *cb_allocator;
	io_request req;
};

/**
 * This slab allocator makes sure all requests in the callback structure
 * are extended requests.
 */
class callback_allocator: public obj_allocator<thread_callback_s>
{
	class callback_initiator: public obj_initiator<thread_callback_s>
	{
	public:
		void init(thread_callback_s *cb) {
			cb->req.init();
		}
	} initiator;
public:
	callback_allocator(int node_id, long increase_size,
			long max_size = MAX_SIZE): obj_allocator<thread_callback_s>(node_id,
				increase_size, max_size, &initiator) {
	}
};

class async_io: public io_interface
{
	int buf_idx;
	struct aio_ctx *ctx;
	callback *cb;
	const int AIO_DEPTH;
	callback_allocator cb_allocator;
	std::tr1::unordered_map<int, aio_complete_sender *> complete_senders;
	std::tr1::unordered_map<int, fifo_queue<thread_callback_s *> *> remote_tcbs;

	int num_iowait;
	int num_completed_reqs;
	int num_local_alloc;

	// file id <-> buffered io
	std::tr1::unordered_map<int, buffered_io *> open_files;
	buffered_io *default_io;

	struct iocb *construct_req(io_request &io_req, callback_t cb_func);
public:
	/**
	 * @aio_depth_per_file
	 * @node_id: the NUMA node where the disks to be read are connected to.
	 */
	async_io(const logical_file_partition &partition,
			const std::tr1::unordered_map<int, aio_complete_thread *> &complete_threads,
			int aio_depth_per_file, int node_id);

	virtual ~async_io();

	virtual io_status access(char *, off_t, ssize_t, int) {
		return IO_UNSUPPORTED;
	}
	virtual void access(io_request *requests, int num, io_status *status = NULL);

	bool set_callback(callback *cb) {
		this->cb = cb;
		return true;
	}

	callback *get_callback() {
		return cb;
	}

	bool support_aio() {
		return true;
	}

	int get_file_id() const {
		if (default_io)
			return default_io->get_file_id();
		else
			return -1;
	}

	virtual void cleanup();

	void return_cb(thread_callback_s *tcbs[], int num);

	int num_available_IO_slots() const {
		return max_io_slot(ctx);
	}

	int num_pending_IOs() const {
		return AIO_DEPTH - max_io_slot(ctx);
	}

	void wait4complete() {
		io_wait(ctx, NULL, 1);
	}

	int get_num_iowait() const {
		return num_iowait;
	}

	int get_num_completed_reqs() const {
		return num_completed_reqs;
	}

	int get_num_local_alloc() const {
		return num_local_alloc;
	}

	virtual void flush_requests() {
		// There is nothing we can flush for incoming requests,
		// but we can flush completed requests.
		for (std::tr1::unordered_map<int, aio_complete_sender *>::iterator it
				= complete_senders.begin(); it != complete_senders.end(); it++) {
			aio_complete_sender *sender = it->second;
			sender->flush(true);
		}
	}

	// These two interfaces allow users to open and close more files.
	
	/*
	 * It opens a virtual file.
	 * Actually, it opens physical files on the underlying filesystems
	 * within the partition of the virtual file, managed by the IO interface.
	 */
	int open_file(const logical_file_partition &partition);
	int close_file(int file_id);
};

class aio_complete_thread: public thread
{
	aio_complete_queue queue;
	int num_completed_reqs;
public:
	aio_complete_thread(int node_id): thread("aio_complete_thread",
			node_id), queue(node_id) {
		num_completed_reqs = 0;
		start();
	}

	void run();

	int get_num_completed_reqs() const {
		return num_completed_reqs;
	}

	aio_complete_queue *get_queue() {
		return &queue;
	}
};

#endif
