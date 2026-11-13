"""Camera paths (debug.camera_path, port/linux/game/camera_pose.c) for the quality gates' captures: every pose and
every console command at a known frame of the path, the same frames in every run, whatever the machine's speed;
only the frames saved go to the stream (the path's save lines).

A capture is a Timeline: commands at the frame they run, the camera held at a pose for some frames (the frame to save
the last of them), or moved along a scene's keys a step at a time. The path's poses between keys follow a
Catmull-Rom spline (camera_pose.c), which passes through each key at its frame but bends between two keys of one
pose next to keys of others; so a held pose has keys at its first two and last two frames (the spline then flat
between them) and a move has a key every frame. pose_at() is camera_pose.c's spline, for checking the file."""

POSE_FORMAT = "%.3f"


def parse_pose(text):
    values = [float(v) for v in text.split()]
    if len(values) != 7 or not 0.0 < values[6] < 90.0:
        raise ValueError(f"a pose is x y z yaw pitch roll field-of-view (under 90): {text!r}")
    return values


def pose_text(pose):
    return " ".join(POSE_FORMAT % v for v in pose)


def lerp_pose(a, b, t):
    return [x + (y - x) * t for x, y in zip(a, b)]


def move_poses(keys, steps):
    """the poses of a move along keys, `steps` a leg, linear (gate2.py's): the first leg's first step is 1/steps
    of the way"""
    poses = []
    for a, b in zip(keys, keys[1:]):
        for i in range(steps):
            poses.append(lerp_pose(a, b, (i + 1) / steps))
    return poses


class Timeline:
    """a path built frame by frame from frame 0"""

    def __init__(self):
        self.frame = 0
        self.keys = {}
        self.commands = []
        self.saves = {}

    def at(self, *commands):
        """console commands run at the current frame, before its pose (camera_pose.c)"""
        for command in commands:
            if len(command) >= 128 or "\n" in command:
                raise ValueError(f"a path's command is one line under 128 bytes: {command!r}")
            self.commands.append((self.frame, command))

    def hold(self, pose, frames, save=None):
        """the camera at a pose for `frames` frames from the current one; the last of them saved as `save`"""
        if frames < 1:
            raise ValueError("a hold of no frames")
        pose = [float(v) for v in pose]
        first, last = self.frame, self.frame + frames - 1
        for frame in sorted({first, min(first + 1, last), max(last - 1, first), last}):
            self.keys[frame] = pose
        if save:
            if save in self.saves.values():
                raise ValueError(f"saved twice: {save}")
            self.saves[last] = save
        self.frame += frames

    def wait(self, frames):
        """frames go by with the camera where the last hold left it"""
        if not self.keys:
            raise ValueError("a wait before any pose")
        self.hold(self.keys[max(self.keys)], frames)

    def text(self, start_frames=0):
        """the path file: its start, its commands (in order) and its keys"""
        if not self.keys:
            raise ValueError("a path with no pose")
        lines = [f"start {start_frames}"]
        lines += [f"at {frame} {command}" for frame, command in self.commands]
        lines += [f"key {frame} {pose_text(pose)}" for frame, pose in sorted(self.keys.items())]
        lines += [f"save {frame}" for frame in sorted(self.saves)]
        return "\n".join(lines) + "\n"

    @property
    def length(self):
        """the path's frames (its last key's frame and one)"""
        return max(self.keys) + 1


def read_keys(text):
    """a path file's keys: [(frame, pose)], as camera_pose.c reads them (each yaw the shorter way round from the
    one before)"""
    keys = []
    for line in text.splitlines():
        parts = line.split()
        if parts[:1] == ["key"] and len(parts) == 9:
            pose = [float(v) for v in parts[2:]]
            if keys:
                before = keys[-1][1][3]
                while pose[3] - before > 180.0:
                    pose[3] -= 360.0
                while pose[3] - before < -180.0:
                    pose[3] += 360.0
            keys.append((int(parts[1]), pose))
    return keys


def pose_at(keys, frame):
    """camera_pose.c's camera_path_pose: the pose at a frame of the path"""
    k = 0
    while k + 1 < len(keys) and keys[k + 1][0] <= frame:
        k += 1
    if k + 1 >= len(keys):
        return list(keys[k][1])
    p0 = keys[k - 1][1] if k > 0 else keys[k][1]
    p1, p2 = keys[k][1], keys[k + 1][1]
    p3 = keys[k + 2][1] if k + 2 < len(keys) else p2
    t = (frame - keys[k][0]) / (keys[k + 1][0] - keys[k][0])
    t2, t3 = t * t, t * t * t
    return [0.5 * (2 * b + (c - a) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (3 * b - a - 3 * c + d) * t3)
            for a, b, c, d in zip(p0, p1, p2, p3)]
