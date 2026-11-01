#pragma once
#include "camera_ctrl.h"
#include "hconfig.h"
#include "hstatus.h"
#include "hrecord.h"
#include "pipeline.h"
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
 *   GET  /camera           camera controls in force here (camera sources)
 *   POST /camera           new ones: applied, kept in camera_file, and
 *                          answered once the camera has taken them
 *
 * Config and eeye's health come from this machine when "eeye_config" is
 * set, else from the peer hellion ("peer"), which serves the same paths.
 * With "camera_controls": "peer" (placement A, topside), the "camera"
 * object of every config eeye accepts is also sent to the peer's /camera,
 * since the camera is there.
 */

typedef struct Control Control;

// Starts listening on cfg->control, and talking to cfg->peer if set.
// `rec` serves /record (owned by the caller). NULL (with a message) on
// failure, or when cfg->control is ""; hellion runs on without its page.
Control *control_start(const HellionConfig *cfg, HRec *rec, Pipeline **pipes,
                       size_t pipe_count);

// The camera controls kept in `path` (written by POST /camera), over the
// defaults (keep_framerate on, as in eeye; nothing else set). A missing
// file is just the defaults; an unreadable or invalid one is too, with
// the reason in err (and false returned).
bool control_load_camera(const char *path, CameraControls *out, char *err,
                         size_t err_cap);

// Fills the status snapshot's peer view. Safe on NULL.
void control_fill(Control *c, HsStatus *s);

// The latest status snapshot, for /api/status, /api/peer and the page.
// Called by the status loop after each publish.
void control_set_status(Control *c, const HsStatus *s);

void control_stop(Control *c);
