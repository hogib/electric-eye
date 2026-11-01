#pragma once
#include "hconfig.h"
#include "hstatus.h"
#include <stdbool.h>

/*
 * hellion's HTTP face: the browser page and its API, and the link to the
 * hellion at the other end of the tether.
 *
 *   GET  /                 the control page
 *   GET  /stream?src=S     MJPEG preview of frame bus stream S
 *   POST /snapshot?src=S   save a JPEG of S's next frame
 *   GET  /config           eeye's config (here, or from the peer)
 *   POST /config           write it; answers with eeye's verdict
 *   GET  /health?src=S     link state of S, eeye's health, recorder state
 *   POST /record           start/stop recording (see recorder)
 *   GET  /api/streams      the frame bus streams on this machine
 *   GET  /api/drone        eeye's health file here, with its age
 *   GET  /api/status       this hellion's status as JSON, for scripts
 *
 * Config and eeye's health come from this machine when "eeye_config" is
 * set, else from the peer hellion ("peer"), which serves the same paths.
 */

typedef struct Control Control;

// Starts listening on cfg->control. NULL (with a message) on failure;
// hellion runs on without its page then.
Control *control_start(const HellionConfig *cfg);

// The latest status snapshot, for /api/status and the page. Called by the
// status loop after each publish.
void control_set_status(Control *c, const HsStatus *s);

void control_stop(Control *c);
