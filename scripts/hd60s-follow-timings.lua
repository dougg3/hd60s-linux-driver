--[[
Keep OBS capture sources for the Elgato HD60 S on the mode the HDMI source is
sending.

The HD60 S is unlike a lot of other capture devices, and only sends out the
specific resolution and timing that it is currently capturing. In other words,
it is not a scaler and can't automatically scale and retime its incoming video
signal.

This presents a problem to OBS, because although OBS supports S_DV_TIMINGS, it
only does it once when the capture starts. If the captured mode changes (for
example due to a console changing between 480i and 480p), the capture keeps
the old geometry and shows garbage or nothing.

This script exists to work around this problem. It checks to see if the
timings have changed, and if so, restarts the source with the new timings.
This effectively helps OBS automatically detect when a change occurs.
]]

local obs = obslua
local ffi = require("ffi")

ffi.cdef [[
int open(const char *path, int flags);
int close(int fd);
int ioctl(int fd, unsigned long req, ...);
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk, struct timespec *ts);
]]

local HD60S_VID = "0fd9"
local HD60S_PIDS = { ["004f"] = true, ["005e"] = true,	-- revisions 1, 2
		     ["0074"] = true, ["0076"] = true }	-- revisions 3, 4

local O_RDWR, O_NONBLOCK = 2, 0x800
local CLOCK_MONOTONIC = 1
local ENODEV, EBUSY = 19, 16
local VIDIOC_G_DV_TIMINGS = 0xC0845658
local VIDIOC_S_DV_TIMINGS = 0xC0845657
local VIDIOC_QUERY_DV_TIMINGS = 0x80845663
local DV_TIMINGS_SIZE = 132

-- Both ioctls read state the driver already holds, so checking often is
-- cheap. The settle time keeps a source that passes through several modes
-- while booting from restarting the capture for each one.
local SETTLE = 0.25	-- seconds a mode must hold before it is adopted
local INTERVAL = 100	-- ms between checks

local DETECTED = 0	-- DV Timings entry; the driver lists only the detected mode
local RESTORED = { "dv_timing", "standard" }

local devices = {}	-- path -> { fd, pending, since, tried }
local restarted = {}	-- source name -> settings to put back next check

-- Seconds on the monotonic clock, for timing how long a mode has held.
local function now()
	local ts = ffi.new("struct timespec")
	ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
	return tonumber(ts.tv_sec) + tonumber(ts.tv_nsec) / 1e9
end

-- Writes to the script log, which OBS also copies into its own log.
local function log(msg)
	obs.script_log(obs.LOG_INFO, msg)
end

-- Reads the 32-bit field at byte offset off of a struct v4l2_dv_timings.
-- The struct is packed, and its bt member starts at offset 4.
local function u32(buf, off)
	return ffi.cast("uint32_t *", buf + off)[0]
end

-- Identifies a mode by what decides the capture format: size, interlacing
-- and pixel clock. For comparing modes; describe() is for the log.
local function key(t)
	return string.format("%dx%d%s@%d", u32(t, 4), u32(t, 8),
			     u32(t, 12) ~= 0 and "i" or "p",
			     u32(t, 20) + u32(t, 24) * 2 ^ 32)
end

-- Formats a mode for the log the way the kernel and OBS's DV Timings list
-- do, such as "1920x1080p 60.00". For interlaced modes the rate is the field
-- rate, as theirs is.
local function describe(t)
	local w = u32(t, 4) + u32(t, 28) + u32(t, 32) + u32(t, 36)
	local h = u32(t, 8) + u32(t, 40) + u32(t, 44) + u32(t, 48) +
		  u32(t, 52) + u32(t, 56) + u32(t, 60)
	local il = u32(t, 12) ~= 0
	local pclk = u32(t, 20) + u32(t, 24) * 2 ^ 32
	return string.format("%dx%d%s %.2f", u32(t, 4), u32(t, 8),
			     il and "i" or "p", pclk / (w * (il and h / 2 or h)))
end

-- Runs a DV timings ioctl that fills in a struct v4l2_dv_timings. Returns
-- the struct, or nil and errno.
local function timings(fd, req)
	local t = ffi.new("uint8_t[?]", DV_TIMINGS_SIZE)
	if ffi.C.ioctl(fd, req, t) < 0 then
		return nil, ffi.errno()
	end
	return t
end

-- Returns the first line of a sysfs file, or nil if it cannot be read.
local function sysfs(file)
	local f = io.open(file)
	if not f then
		return nil
	end
	local v = f:read("*l")
	f:close()
	return v
end

-- Whether a device node belongs to an HD60 S, from the USB IDs of its parent
-- device.
local function is_hd60s(path)
	local node = path:match("[^/]+$")
	if not node then
		return false
	end
	local dir = "/sys/class/video4linux/" .. node .. "/device/../"
	return sysfs(dir .. "idVendor") == HD60S_VID and
	       HD60S_PIDS[sysfs(dir .. "idProduct")] == true
end

-- Returns the state kept for an HD60 S device node, opening the node on first
-- use. Returns nil if the node is not an HD60 S or cannot be opened.
local function device(path)
	local d = devices[path]
	if d then
		return d
	end
	if not is_hd60s(path) then
		return nil
	end
	local fd = ffi.C.open(path, bit.bor(O_RDWR, O_NONBLOCK))
	if fd < 0 then
		return nil
	end
	d = { fd = fd }
	devices[path] = d
	return d
end

-- Closes a device node that has been unplugged. The next device() reopens
-- whatever the path names by then.
local function forget(path)
	ffi.C.close(devices[path].fd)
	devices[path] = nil
end

-- Restarts an OBS capture source so that it reopens with S_DV_TIMINGS for the
-- detected mode. OBS restarts a V4L2 capture only when a setting differs from
-- the copy it last applied, and its copy of DV Timings may already be the
-- detected mode, so the restart is forced through "standard": OBS compares it
-- too, but applies it only to inputs with analog standards, which the HD60 S
-- does not have.
--
-- Returns what restore() needs to put the user's settings back: the value,
-- or false where the user never set one and the default applies.
local function restart_with_detected(name)
	local src = obs.obs_get_source_by_name(name)
	if not src then
		return nil
	end
	local cur = obs.obs_source_get_settings(src)
	local saved = {}
	for _, k in ipairs(RESTORED) do
		saved[k] = obs.obs_data_has_user_value(cur, k) and
			   obs.obs_data_get_int(cur, k) or false
	end
	obs.obs_data_release(cur)

	-- Never used before, so it always differs from OBS's copy. Negative, so
	-- never a valid standard, and within 32 bits because OBS stores it in an
	-- int.
	local nonce = -2 - math.floor(now() * 1000) % 2 ^ 30

	local s = obs.obs_data_create()
	obs.obs_data_set_int(s, "dv_timing", DETECTED)
	obs.obs_data_set_int(s, "standard", nonce)
	obs.obs_source_update(src, s)
	obs.obs_data_release(s)
	obs.obs_source_release(src)
	return saved
end

-- Puts back the settings restart_with_detected() changed. They are written
-- into the source's own settings rather than through an update, so the
-- capture is not restarted again.
local function restore(name, saved)
	local src = obs.obs_get_source_by_name(name)
	if not src then
		return
	end
	local s = obs.obs_source_get_settings(src)
	for _, k in ipairs(RESTORED) do
		if saved[k] then
			obs.obs_data_set_int(s, k, saved[k])
		else
			obs.obs_data_unset_user_value(s, k)
		end
	end
	obs.obs_data_release(s)
	obs.obs_source_release(src)
end

-- Lists every V4L2 capture source in OBS with its name and device node.
local function v4l2_sources()
	local found = {}
	local list = obs.obs_enum_sources()
	for _, src in ipairs(list or {}) do
		if obs.obs_source_get_unversioned_id(src) == "v4l2_input" then
			local s = obs.obs_source_get_settings(src)
			table.insert(found, {
				name = obs.obs_source_get_name(src),
				path = obs.obs_data_get_string(s, "device_id"),
			})
			obs.obs_data_release(s)
		end
	end
	obs.source_list_release(list)
	return found
end

-- Runs every INTERVAL ms. Puts back the settings of the previous check's
-- restarts, then compares the detected and configured timings of each
-- capture source's HD60 S. A mode that has held for SETTLE seconds is set on
-- the device, or, if a capture holds the device's buffers, by restarting the
-- capture sources that use it.
local function check()
	-- Not in the same check as the restart: OBS applies the update when it
	-- ticks the sources, after this timer in the same video tick, so
	-- restoring in this check would undo it.
	for name, saved in pairs(restarted) do
		restore(name, saved)
		restarted[name] = nil
	end

	local sources = v4l2_sources()
	local seen = {}
	for _, src in ipairs(sources) do
		local d = src.path ~= "" and not seen[src.path] and device(src.path)
		seen[src.path] = true
		if d then
			local detected, err = timings(d.fd, VIDIOC_QUERY_DV_TIMINGS)
			local configured = timings(d.fd, VIDIOC_G_DV_TIMINGS)

			if err == ENODEV then
				forget(src.path)
			elseif not detected or not configured or
			   key(detected) == key(configured) then
				d.pending, d.tried = nil, nil
			elseif d.pending ~= key(detected) then
				d.pending, d.since = key(detected), now()
			elseif now() - d.since >= SETTLE and d.tried ~= d.pending then
				local from, to = describe(configured), describe(detected)

				-- Once per mode, whatever the outcome: if the buffers
				-- belong to something other than OBS, restarting
				-- OBS's capture sources will not free them.
				d.tried = d.pending
				if ffi.C.ioctl(d.fd, VIDIOC_S_DV_TIMINGS, detected) == 0 then
					log(src.path .. ": " .. from .. " -> " .. to)
				elseif ffi.errno() == EBUSY then
					for _, s in ipairs(sources) do
						if s.path == src.path then
							log(s.name .. ": " .. from .. " -> " .. to ..
							    ", restarting the capture source")
							restarted[s.name] = restart_with_detected(s.name)
						end
					end
				else
					log(src.path .. ": S_DV_TIMINGS failed, errno " .. ffi.errno())
				end
			end
		end
	end
end

-- Shown in OBS's Tools > Scripts window.
function script_description()
	return "Follows mode changes of the HDMI source on Elgato HD60 S " ..
	       "capture sources, by updating the DV timings and restarting " ..
	       "the capture source."
end

-- Called by OBS when the script is loaded: starts the checks.
function script_load(settings)
	obs.timer_add(check, INTERVAL)
end

-- Called by OBS when the script is unloaded or OBS exits: stops the checks,
-- puts back any settings still pending, and closes the device nodes.
function script_unload()
	obs.timer_remove(check)
	-- A restart still waiting to be put back would otherwise leave the
	-- forced settings in the user's scene.
	for name, saved in pairs(restarted) do
		restore(name, saved)
	end
	restarted = {}
	for _, d in pairs(devices) do
		ffi.C.close(d.fd)
	end
	devices = {}
end
