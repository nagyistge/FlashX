#ifndef __RAW_DATA_ARRAY_H__
#define __RAW_DATA_ARRAY_H__

/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <assert.h>

#include <memory>

namespace fm
{

namespace detail
{

/*
 * This class holds the allocated memory and helps other classes access
 * the data in the memory. We shouldn't pass the object of this class
 * around, which is relatively expensive.
 */
class raw_data_array
{
	int node_id;
	// The total number of bytes in the allocated memory.
	size_t num_bytes;
	// This points to the beginning of the allocated memory.
	std::shared_ptr<char> data;

	// The number of bytes in the allocated memory exposed to a user.
	size_t num_used_bytes;
	// This points to the start address of the array exposed to a user.
	char *start;
public:
	raw_data_array() {
		node_id = -1;
		num_bytes = 0;
		start = NULL;
		num_used_bytes = 0;
	}

	raw_data_array(size_t num_bytes, int node_id);

	/*
	 * This gets the offset of the start pointer in the allocated memory.
	 */
	off_t get_ptr_off() const {
		return start - data.get();
	}

	/*
	 * Move the start pointer to the specified offset in the allocated memory.
	 */
	void move_pointer(size_t off) {
		start = data.get() + off;
	}

	/*
	 * Get the number of bytes exposed to a user.
	 */
	size_t get_num_bytes() const {
		return num_used_bytes;
	}

	/*
	 * Test if a user can access the entire memory.
	 */
	bool has_entire_array() const {
		return num_used_bytes == num_bytes;
	}

	void reset_data() {
		// Let's control data access here. We should avoid subarray from
		// modifying the data array accidently.
		assert(has_entire_array());
		memset(data.get(), 0, num_bytes);
	}

	char *get_raw() {
		assert(has_entire_array());
		return start;
	}

	const char *get_raw() const {
		return start;
	}

	int get_node_id() const {
		return node_id;
	}
};

}

}

#endif