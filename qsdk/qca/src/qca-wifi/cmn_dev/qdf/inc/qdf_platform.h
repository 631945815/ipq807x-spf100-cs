/*
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: qdf_platform.h
 * This file defines platform API abstractions.
 */

#ifndef _QDF_PLATFORM_H
#define _QDF_PLATFORM_H

/**
 * qdf_self_recovery_callback() - callback for self recovery
 * @reason: the reason for the recovery request
 * @func: the caller's function name
 * @line: the line number of the callsite
 *
 * Return: none
 */
typedef void (*qdf_self_recovery_callback)(enum qdf_hang_reason reason,
					   const char *func,
					   const uint32_t line);

/**
 * qdf_is_fw_down_callback() - callback to query if fw is down
 *
 * Return: true if fw is down and false if fw is not down
 */
typedef bool (*qdf_is_fw_down_callback)(void);

/**
 * qdf_register_fw_down_callback() - API to register fw down callback
 * @is_fw_down: callback to query if fw is down or not
 *
 * Return: none
 */
void qdf_register_fw_down_callback(qdf_is_fw_down_callback is_fw_down);

/**
 * qdf_is_fw_down() - API to check if fw is down or not
 *
 * Return: true: if fw is down
 *	   false: if fw is not down
 */
bool qdf_is_fw_down(void);

/**
 * qdf_register_self_recovery_callback() - register self recovery callback
 * @callback:  self recovery callback
 *
 * Return: None
 */
void qdf_register_self_recovery_callback(qdf_self_recovery_callback callback);

/**
 * qdf_trigger_self_recovery () - trigger self recovery
 *
 * Call API only in case of fatal error,
 * if self_recovery_cb callback is registered, injcets fw crash and recovers
 * else raises QDF_BUG()
 *
 * Return: None
 */
#define qdf_trigger_self_recovery() \
	__qdf_trigger_self_recovery(__func__, __LINE__)
void __qdf_trigger_self_recovery(const char *func, const uint32_t line);

/**
 * qdf_is_recovering_callback() - callback to get driver recovering in progress
 * or not
 *
 * Return: true if driver is doing recovering else false
 */
typedef bool (*qdf_is_recovering_callback)(void);

/**
 * qdf_register_recovering_state_query_callback() - register recover status
 * query callback
 *
 * Return: none
 */
void qdf_register_recovering_state_query_callback(
	qdf_is_recovering_callback is_recovering);

/**
 * qdf_is_recovering() - get driver recovering in progress status
 * or not
 *
 * Return: true if driver is doing recovering else false
 */
bool qdf_is_recovering(void);

/**
 * struct qdf_op_sync - opaque operation synchronization context handle
 */
struct qdf_op_sync;

typedef int (*qdf_op_protect_cb)(void **out_sync, const char *func);
typedef void (*qdf_op_unprotect_cb)(void *sync, const char *func);

/**
 * qdf_op_protect() - attempt to protect a driver operation
 * @out_sync: output parameter for the synchronization context, populated on
 *	success
 *
 * Return: Errno
 */
#define qdf_op_protect(out_sync) __qdf_op_protect(out_sync, __func__)

qdf_must_check int
__qdf_op_protect(struct qdf_op_sync **out_sync, const char *func);

/**
 * qdf_op_unprotect() - release driver operation protection
 * @sync: synchronization context returned from qdf_op_protect()
 *
 * Return: None
 */
#define qdf_op_unprotect(sync) __qdf_op_unprotect(sync, __func__)

void __qdf_op_unprotect(struct qdf_op_sync *sync, const char *func);

/**
 * qdf_op_callbacks_register() - register driver operation protection callbacks
 *
 * Return: None
 */
void qdf_op_callbacks_register(qdf_op_protect_cb on_protect,
			       qdf_op_unprotect_cb on_unprotect);

#endif /*_QDF_PLATFORM_H*/

