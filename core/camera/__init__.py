# SPDX-License-Identifier: GPL-3.0-only
"""Camera across Beamer: the Python control plane (dev/camera/PHASE2.md §7).

`model` holds the snapshot types the pages read; `part.CameraPart` runs the camera control plane
inside the AudioService worker, through a narrow `MediaContext`.
"""

from core.camera.model import (Camera, CameraEntry, CameraRef, CameraRoute, CameraSnapshot, CameraStatus,
                               Capture, LocalVcam, MachineCamera, QualityEntry, Stamp, VideoStreamView,
                               VirtualCamera)
from core.camera.part import CameraPart, MediaContext

__all__ = ["CameraPart", "MediaContext", "Camera", "CameraEntry", "CameraRef", "CameraRoute", "CameraSnapshot", "CameraStatus", "Capture",
           "LocalVcam", "MachineCamera", "QualityEntry", "Stamp", "VideoStreamView", "VirtualCamera"]
