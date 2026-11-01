#pragma once
#include "camera_ctrl.h"
#include "hconfig.h"
#include "hstatus.h"
#include <stdbool.h>

/*
 * A pipeline: one source thread feeding up to 8 sinks.
 *
 * The source thread owns its source (a camera, or a frame bus stream),
 * reopens it when it goes away, and hands every frame to each sink. Each
 * sink runs on its own thread behind a one-slot mailbox: handing a frame
 * over is a copy into the slot, and if the sink has not taken the last one
 * yet, the newer frame replaces it (counted as dropped for that sink). So
 * a slow sink -- an encoder falling behind, a full disk -- costs that sink
 * frames and never holds up the source or the other sinks.
 *
 * Geometry comes from the source when it opens. If it reopens at a
 * different size (a camera renegotiated, a bus stream's writer restarted
 * at a new size), the sinks are restarted at the new size.
 */

typedef struct Pipeline Pipeline;

// Starts the pipeline's threads. They run until pipeline_stop().
Pipeline *pipeline_start(const PipelineConfig *cfg);

// The same, for a camera source, opening the camera with `camera` (NULL:
// no controls) rather than applying them a moment later -- which for the
// Pi camera would mean restarting rpicam-vid straight away.
Pipeline *pipeline_start_with_camera(const PipelineConfig *cfg,
                                     const CameraControls *camera);

// Whether the source is a camera (v4l2: or rpicam), the only kind camera
// controls apply to.
bool pipeline_has_camera(const Pipeline *p);

// New camera controls. The source thread applies them before its next
// frame: ioctls on a V4L2 camera; for the Pi camera, a restart of
// rpicam-vid (a ~1s gap; rpicam takes controls only on its command line).
// The same controls again change nothing. They are also what the camera
// is opened with after a reconnect.
void pipeline_set_camera(Pipeline *p, const CameraControls *c);

// Waits up to timeout_ms for the latest controls to be applied. Returns 1
// when they were (*applied: how many the camera accepted), 0 when the
// camera is not open (they apply when it opens), -1 on timeout.
int pipeline_camera_wait(Pipeline *p, int timeout_ms, int *applied);

// Fills `out` for the status file, including fps since the last call.
void pipeline_status(Pipeline *p, HsPipeline *out);

// The relay an RTP source receives through (NULL for other sources, or
// before it is set up). Valid until pipeline_stop(); the recorder attaches
// to it.
struct UdpRelay *pipeline_rtp_relay(Pipeline *p);

const PipelineConfig *pipeline_config(const Pipeline *p);

// Stops every thread and releases the source and sinks.
void pipeline_stop(Pipeline *p);
