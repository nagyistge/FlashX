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

#include <omp.h>

#include <atomic>

#include "log.h"

#include "mem_dense_matrix.h"

namespace fm
{

const size_t SUB_CHUNK_SIZE = 1024;

class sub_matrix
{
	size_t start_row;
	size_t start_col;
	size_t nrow;
	size_t ncol;
public:
	sub_matrix(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol) {
		this->start_row = start_row;
		this->start_col = start_col;
		this->nrow = nrow;
		this->ncol = ncol;
	}

	size_t get_num_rows() const {
		return nrow;
	}

	size_t get_num_cols() const {
		return ncol;
	}

	size_t get_start_row() const {
		return start_row;
	}

	size_t get_start_col() const {
		return start_col;
	}
};

class sub_col_matrix: public sub_matrix
{
	const mem_col_dense_matrix &m;
public:
	sub_col_matrix(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol, const mem_col_dense_matrix &_m): sub_matrix(
				start_row, nrow, start_col, ncol), m(_m) {
		assert(start_row + nrow <= m.get_num_rows());
		assert(start_col + ncol <= m.get_num_cols());
	}

	const char *get_col(size_t col) const {
		return m.get_col(get_start_col() + col)
			+ get_start_row() * m.get_entry_size();
	}
};

class sub_row_matrix: public sub_matrix
{
	const mem_row_dense_matrix &m;
public:
	sub_row_matrix(size_t start_row, size_t nrow, size_t start_col,
			size_t ncol, const mem_row_dense_matrix &_m): sub_matrix(
				start_row, nrow, start_col, ncol), m(_m) {
		assert(start_row + nrow <= m.get_num_rows());
		assert(start_col + ncol <= m.get_num_cols());
	}

	const char *get_row(size_t row) const {
		return m.get_row(get_start_row() + row) + get_start_col() * m.get_entry_size();
	}
};

bool mem_dense_matrix::verify_inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (this->get_entry_size() != left_op.left_entry_size()
			|| m.get_entry_size() != left_op.right_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The left operator isn't compatible with input matrices";
		return false;
	}

	if (left_op.output_entry_size() != right_op.left_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The type of the left operator doesn't match the right operator";
		return false;
	}

	if (right_op.left_entry_size() != right_op.right_entry_size()
			|| right_op.left_entry_size() != right_op.output_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The input and output of the right operator has different types";
		return false;
	}

	if (get_num_cols() != m.get_num_rows()) {
		BOOST_LOG_TRIVIAL(error) << "The matrix size doesn't match";
		return false;
	}
	return true;
}

void mem_col_dense_matrix::reset_data()
{
	size_t tot_bytes = get_num_rows() * get_num_cols() * get_entry_size();
	memset(data, 0, tot_bytes);
}

void mem_col_dense_matrix::set_data(const set_operate &op)
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	for (size_t i = 0; i < ncol; i++)
		op.set(get_col(i), nrow, 0, i);
}

void mem_col_dense_matrix::par_reset_data()
{
	size_t tot_bytes = get_num_rows() * get_num_cols() * get_entry_size();
#pragma omp parallel for
	for (size_t i = 0; i < tot_bytes; i += PAGE_SIZE)
		memset(data + i, 0, std::min(tot_bytes - i, (size_t) PAGE_SIZE));
}

void mem_col_dense_matrix::par_set_data(const set_operate &op)
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
#pragma omp parallel for
	for (size_t i = 0; i < ncol; i++)
		op.set(get_col(i), nrow, 0, i);
}

mem_dense_matrix::ptr mem_col_dense_matrix::inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (!verify_inner_prod(m, left_op, right_op))
		return mem_dense_matrix::ptr();

	size_t ncol = this->get_num_cols();
	size_t nrow = this->get_num_rows();
	assert(nrow > ncol);
	mem_col_dense_matrix::ptr res = mem_col_dense_matrix::create(nrow,
			m.get_num_cols(), right_op.output_entry_size());
	res->reset_data();

	char *tmp_res = (char *) malloc(SUB_CHUNK_SIZE * res->get_entry_size());
	for (size_t k = 0; k < nrow; k += SUB_CHUNK_SIZE) {
		sub_col_matrix subm(k, std::min(SUB_CHUNK_SIZE, nrow - k), 0, ncol, *this);
		for (size_t i = 0; i < ncol; i++) {
			for (size_t j = 0; j < m.get_num_cols(); j++) {
				left_op.runAE(subm.get_num_rows(), subm.get_col(i),
						m.get(i, j), tmp_res);
				char *store_col = res->get_col(j) + k * res->get_entry_size();
				right_op.runAA(subm.get_num_rows(), tmp_res, store_col,
						store_col);
			}
		}
	}
	free(tmp_res);
	return res;
}

mem_dense_matrix::ptr mem_col_dense_matrix::par_inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (!verify_inner_prod(m, left_op, right_op))
		return mem_dense_matrix::ptr();

	size_t ncol = this->get_num_cols();
	size_t nrow = this->get_num_rows();
	assert(nrow > ncol);
	mem_col_dense_matrix::ptr res = mem_col_dense_matrix::create(nrow,
			m.get_num_cols(), right_op.output_entry_size());
	res->par_reset_data();

#pragma omp parallel
	{
		char *tmp_res = (char *) malloc(SUB_CHUNK_SIZE * res->get_entry_size());
#pragma omp for
		for (size_t k = 0; k < nrow; k += SUB_CHUNK_SIZE) {
			sub_col_matrix subm(k, std::min(SUB_CHUNK_SIZE, nrow - k), 0, ncol, *this);
			for (size_t i = 0; i < ncol; i++) {
				for (size_t j = 0; j < m.get_num_cols(); j++) {
					left_op.runAE(subm.get_num_rows(), subm.get_col(i),
							m.get(i, j), tmp_res);
					char *store_col = res->get_col(j) + k * res->get_entry_size();
					right_op.runAA(subm.get_num_rows(), tmp_res, store_col,
							store_col);
				}
			}
		}
		free(tmp_res);
	}
	return res;
}

void mem_row_dense_matrix::reset_data()
{
	size_t tot_bytes = get_num_rows() * get_num_cols() * get_entry_size();
	memset(data, 0, tot_bytes);
}

void mem_row_dense_matrix::set_data(const set_operate &op)
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
	for (size_t i = 0; i < nrow; i++)
		op.set(get_row(i), ncol, i, 0);
}

void mem_row_dense_matrix::par_reset_data()
{
	size_t tot_bytes = get_num_rows() * get_num_cols() * get_entry_size();
#pragma omp parallel for
	for (size_t i = 0; i < tot_bytes; i += PAGE_SIZE)
		memset(data + i, 0, std::min(tot_bytes - i, (size_t) PAGE_SIZE));
}

void mem_row_dense_matrix::par_set_data(const set_operate &op)
{
	size_t ncol = get_num_cols();
	size_t nrow = get_num_rows();
#pragma omp parallel for
	for (size_t i = 0; i < nrow; i++)
		op.set(get_row(i), ncol, i, 0);
}

bool mem_row_dense_matrix::verify_inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (m.store_layout() != matrix_layout_t::L_COL) {
		BOOST_LOG_TRIVIAL(error)
			<< "The layout of the right matrix has to be column matrix";
		return false;
	}
	return mem_dense_matrix::verify_inner_prod(m, left_op, right_op);
}

mem_dense_matrix::ptr mem_row_dense_matrix::inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (!verify_inner_prod(m, left_op, right_op))
		return mem_dense_matrix::ptr();
	assert(m.store_layout() == matrix_layout_t::L_COL);

	size_t ncol = this->get_num_cols();
	size_t nrow = this->get_num_rows();
	assert(ncol > nrow);
	mem_row_dense_matrix::ptr res = mem_row_dense_matrix::create(nrow,
			m.get_num_cols(), right_op.output_entry_size());
	res->reset_data();

	const mem_col_dense_matrix &col_m = (const mem_col_dense_matrix &) m;
	char *tmp_res = (char *) malloc(SUB_CHUNK_SIZE * left_op.output_entry_size());
	char *tmp_res2 = (char *) malloc(res->get_num_cols() * res->get_entry_size());
	for (size_t k = 0; k < ncol; k += SUB_CHUNK_SIZE) {
		size_t sub_ncol = std::min(SUB_CHUNK_SIZE, ncol - k);
		sub_row_matrix sub_left(0, nrow, k, sub_ncol, *this);
		sub_col_matrix sub_right(k, sub_ncol, 0, m.get_num_cols(), col_m);
		for (size_t i = 0; i < sub_left.get_num_rows(); i++) {
			for (size_t j = 0; j < sub_right.get_num_cols(); j++) {
				left_op.runAA(sub_ncol, sub_left.get_row(i),
						sub_right.get_col(j), tmp_res);
				right_op.runA(sub_ncol, tmp_res,
						tmp_res2 + res->get_entry_size() * j);
			}
			// This is fine because we assume the input type of the right operator
			// should be the same as the type of the output matrix.
			right_op.runAA(sub_right.get_num_cols(), tmp_res2, res->get_row(i),
					res->get_row(i));
		}
	}
	return res;
}

static int get_num_omp_threads()
{
	std::atomic<int> num_threads;
#pragma omp parallel
	{
		num_threads = omp_get_num_threads();
	}
	return num_threads.load();
}

mem_dense_matrix::ptr mem_row_dense_matrix::par_inner_prod(const mem_dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (!verify_inner_prod(m, left_op, right_op))
		return mem_dense_matrix::ptr();
	assert(m.store_layout() == matrix_layout_t::L_COL);

	const mem_col_dense_matrix &col_m = (const mem_col_dense_matrix &) m;
	size_t ncol = this->get_num_cols();
	size_t nrow = this->get_num_rows();
	assert(ncol > nrow);
	mem_row_dense_matrix::ptr res = mem_row_dense_matrix::create(nrow,
			m.get_num_cols(), right_op.output_entry_size());
	res->par_reset_data();

	int nthreads = get_num_omp_threads();
	std::vector<mem_row_dense_matrix::ptr> local_ms(nthreads);

#pragma omp parallel
	{
		char *tmp_res = (char *) malloc(
				SUB_CHUNK_SIZE * left_op.output_entry_size());
		char *tmp_res2 = (char *) malloc(
				res->get_num_cols() * res->get_entry_size());
		mem_row_dense_matrix::ptr local_m = mem_row_dense_matrix::create(nrow,
				m.get_num_cols(), right_op.output_entry_size());
#pragma omp for
		for (size_t k = 0; k < ncol; k += SUB_CHUNK_SIZE) {
			size_t sub_ncol = std::min(SUB_CHUNK_SIZE, ncol - k);
			sub_row_matrix sub_left(0, nrow, k, sub_ncol, *this);
			sub_col_matrix sub_right(k, sub_ncol, 0, m.get_num_cols(), col_m);
			for (size_t i = 0; i < sub_left.get_num_rows(); i++) {
				for (size_t j = 0; j < sub_right.get_num_cols(); j++) {
					left_op.runAA(sub_ncol, sub_left.get_row(i),
							sub_right.get_col(j), tmp_res);
					right_op.runA(sub_ncol, tmp_res,
							tmp_res2 + res->get_entry_size() * j);
				}
				// This is fine because we assume the input type and output type of
				// the right operator should be the same as the type of the output
				// matrix.
				right_op.runAA(sub_right.get_num_cols(), tmp_res2,
						local_m->get_row(i),
						local_m->get_row(i));
			}
		}
		local_ms[omp_get_thread_num()] = local_m;
	}

	// Aggregate the results from omp threads.
	for (size_t i = 0; i < res->get_num_rows(); i++) {
		for (int j = 0; j < nthreads; j++) {
			right_op.runAA(res->get_num_cols(), local_ms[j]->get_row(i),
					res->get_row(i), res->get_row(i));
		}
	}

	return res;
}

}
