# name: Selenograph
# summary: Tonight's real moon phase, worked out from the date alone - then the story on a click.
# author: Stipple
# tags: time, space, astronomy, ambient
# panel: 52x16

import math

class Selenograph
  var COS, HW, NAME
  var phase, pt, txt, base, snd

  def init()
	self.COS = [1000, 981, 924, 831, 707, 556, 383, 195, 0, -195, -383, -556, -707, -831, -924, -981, -1000, -981, -924, -831, -707, -556, -383, -195, 0, 195, 383, 556, 707, 831, 924, 981]
	self.HW = [1, 2, 3, 3, 3, 3, 2, 1]
	self.NAME = ["NEW MOON", "WAXING CRESCENT", "FIRST QUARTER", "WAXING GIBBOUS", "FULL MOON", "WANING GIBBOUS", "LAST QUARTER", "WANING CRESCENT"]
	self.snd = store.get("sound", false)
	self.phase = 0
	self.pt = 0
	self.base = -1
	self.txt = ""
  end

  # Julian Day Number for a civil date (Fliegel-Van Flandern).
  def _jdn(Y, M, D)
	var a = (14 - M) / 12
	var y = Y + 4800 - a
	var m = M + 12 * a - 3
	return D + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045
  end

  # Position in the synodic month as a 0..31 index (0 = new, 16 = full).
  def _idx()
	var Y = year()
	if Y < 2000
	  return -1
	end
	var t = (self._jdn(Y, month(), day()) - 2451550) * 10000 + hour() * 10000 / 24 - 1000
	var p = t % 295306
	if p < 0
	  p += 295306
	end
	return p * 32 / 295306
  end

  # Convert a count of 1/32-month steps into whole days.
  def _round(steps)
	return (steps * 2953 + 1600) / 3200
  end

  # Draw the lit disc: the terminator sweeps across using the cosine table,
  # left-to-right as the moon waxes and back again as it wanes.
  def _disc(idx)
	var k = self.COS[idx % 32]
	var y = 0
	while y < 8
	  var w = self.HW[y]
	  var t = w * k / 1000
	  var dx = -w
	  while dx <= w
		var lit = false
		if idx == 16
		  lit = true
		end
		if idx > 0
		  if idx < 16
			if dx > t
			  lit = true
			end
		  end
		end
		if idx > 16
		  if dx < (0 - t)
			lit = true
		  end
		end
		var c = 0x241F14
		if lit
		  c = 0xF2E3C8
		end
		pixel(4 + dx, y, c)
		dx += 1
	  end
	  y += 1
	end
  end

  # A press builds the scrolling caption and switches into scroll mode.
  def on_button(b)
	if b != "select"
	  return
	end
	var idx = self._idx()
	if idx < 0
	  return
	end
	var ill = (1000 - self.COS[idx % 32]) * 100 / 2000
	var df = self._round((16 - idx + 32) % 32)
	var dn = self._round((32 - idx) % 32)
	var ev = "NEXT FULL MOON IN " + str(df) + " DAYS"
	if dn < df
	  ev = "NEXT NEW MOON IN " + str(dn) + " DAYS"
	end
	self.txt = self.NAME[((idx + 2) % 32) / 4] + "  " + str(ill) + "% ILLUMINATED  " + ev
	self.base = -1
	self.phase = 2
  end

  def duration()
	return 15000
  end

  def draw()
	clear()
	var now = now_ms()
	var idx = self._idx()
	if idx < 0
	  var w = "NO CLOCK"
	  text((width() - text_ink_width(w)) / 2, 6, w, 0x9AA0A6)
	  return
	end

	# Scroll the caption until it has made one full pass, then resume cycling.
	if self.phase == 2
	  var laps = scroll_text(self.txt, 0xF2E3C8)
	  if self.base < 0
		self.base = laps
	  else
		if laps > self.base
		  self.phase = 0
		  self.pt = now
		end
	  end
	  return
	end

	self._disc(idx)
	var df = self._round((16 - idx + 32) % 32)
	var dn = self._round((32 - idx) % 32)
	var ev = "FULL"
	var d = df
	if dn < df
	  ev = "NEW"
	  d = dn
	end
	var s = str((1000 - self.COS[idx % 32]) * 100 / 2000) + "%"
	if self.phase == 1
	  s = ev
	end
	if self.phase == 2
	  s = str(d) + "D"
	  if d == 0
		s = "NOW"
	  end
	end
	text(10, 6, s, 0x9AA0A6)

	# Every 2.2s rotate the readout: illumination -> next event -> days away.
	if now - self.pt > 2200
	  self.pt = now
	  self.phase = (self.phase + 1) % 3
	end
  end
end

return Selenograph()
