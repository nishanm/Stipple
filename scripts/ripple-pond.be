# name: Ripple Pond
# summary: A pond of light. Steer the drop point with the knob and the - and + buttons, press to let one fall.
# author: Stipple
# tags: interactive, generative, colour
# panel: 52x16

# @input exclusive

import math

# Each press drops a ring that spreads and fades. Rings are drawn by walking
# their circumference rather than testing every pixel against every ring,
# which keeps the cost proportional to what is on screen.
class App
  var rings
  var cx, cy
  var hue
  var last

  def init()
	self.rings = []
	self.cx = width() / 2
	self.cy = height() / 2
	self.hue = 0
	self.last = nil
  end

  def _wheel(pos, k)
	pos = pos % 256
	var r = 0
	var g = 0
	var b = 0
	if pos < 85
	  r = 255 - pos * 3
	  g = pos * 3
	elif pos < 170
	  pos -= 85
	  g = 255 - pos * 3
	  b = pos * 3
	else
	  pos -= 170
	  r = pos * 3
	  b = 255 - pos * 3
	end
	return rgb(int(r * k), int(g * k), int(b * k))
  end

  def _clamp()
	if self.cx < 0 self.cx = 0 end
	if self.cx > width() - 1 self.cx = width() - 1 end
	if self.cy < 0 self.cy = 0 end
	if self.cy > height() - 1 self.cy = height() - 1 end
  end

  def on_button(name)
	if name == 'left'
	  self.cx -= 2
	elif name == 'right'
	  self.cx += 2
	elif name == 'plus'
	  self.cy -= 1
	elif name == 'minus'
	  self.cy += 1
	elif name == 'select'
	  if self.rings.size() >= 8
		self.rings.remove(0)
	  end
	  self.rings.push([self.cx, self.cy, 0.0, self.hue])
	  self.hue = (self.hue + 37) % 256
	end
	self._clamp()
  end

  def draw()
	var now = now_ms()
	if self.last == nil self.last = now end
	var dt = (now - self.last) / 1000.0
	self.last = now
	if dt > 0.2 dt = 0.2 end

	var alive = []
	for ring : self.rings
	  ring[2] += dt * 14.0
	  var r = ring[2]
	  var fade = 1.0 - r / 30.0
	  if fade > 0.0
		alive.push(ring)
		var steps = int(r * 6) + 8
		for s : 0 .. steps - 1
		  var a = 6.2831853 * s / steps
		  var x = int(ring[0] + math.cos(a) * r + 0.5)
		  var y = int(ring[1] + math.sin(a) * r + 0.5)
		  if x >= 0 && x < width() && y >= 0 && y < height()
			pixel(x, y, self._wheel(ring[3] + int(r * 3), fade * fade))
		  end
		end
	  end
	end
	self.rings = alive

	# The drop point, pulsing gently.
	var c = int(255 * (0.4 + 0.35 + 0.35 * math.sin(now / 180.0)))
	pixel(self.cx, self.cy, rgb(c, c, c))
  end
end

return App()
