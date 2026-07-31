/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SGWC_S11_BUILD_H
#define SGWC_S11_BUILD_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

ogs_pkbuf_t *sgwc_s11_build_create_session_response(
        uint8_t type, sgwc_sess_t *sess);

ogs_pkbuf_t *sgwc_s11_build_downlink_data_notification(
        uint8_t cause_value, sgwc_bearer_t *bearer);

/*
 * Phase 4 Step 4-5: Build a Create Bearer Response for failure cases.
 *
 * TS 29.274 7.2.4 marks Bearer Context as Mandatory in CBResp.
 * For failure relays (MME rejected E-RAB Setup), the SGW must forward
 * a proper CBResp with at least:
 *   - Cause (top-level) = failure cause
 *   - Bearer Context = { EBI, bearer-level Cause }
 * The previous use of ogs_gtp_send_error_message() omitted Bearer
 * Context, which is non-compliant and causes the receiving SMF to log
 * "No Bearer / No EPS Bearer ID / No Bearer Cause" before falling back
 * to xact-based cleanup.
 */
ogs_pkbuf_t *sgwc_s11_build_create_bearer_response_failure(
        uint8_t type, uint8_t cause_value,
        uint8_t ebi, uint8_t bearer_cause_value);

#ifdef __cplusplus
}
#endif

#endif /* SGWC_S11_BUILD_H */
