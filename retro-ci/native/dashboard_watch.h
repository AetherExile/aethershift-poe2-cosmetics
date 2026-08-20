/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Remove the old RTPN00001 deeplink registration before launcher.c writes and
 * registers the same title again as a real Games title. The Sony eboot host is
 * still prepared by the already-proven hbldr_prepare_host() path. */
int rp_prepare_real_title_registration(void);

/* Start the watcher that detects a genuine dashboard launch of RTPN00001. */
int rp_dashboard_watch_start(void);

/* Mark hbldr transitions initiated by Retro Papa itself. While this window is
 * active, the watcher must not interpret RTPN00001 process churn as a new
 * dashboard click. Calls may be nested. */
void rp_dashboard_internal_begin(void);
void rp_dashboard_internal_end(void);
