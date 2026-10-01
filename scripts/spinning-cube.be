# name: Spinning Cube
# summary: A wireframe cube tumbling in 3D, with depth-shaded edges and a slow colour drift.
# author: Stipple
# tags: animation, 3d, retro
# panel: 52x16

import math

class App
  var ax
  var ay
  var last
  var vx
  var vy
  var vz

  def init()
	self.ax = 0.0
	self.ay = 0.0
	self.last = now_ms()
	self.vx = [-1, 1, 1, -1, -1, 1, 1, -1]
	self.vy = [-1, -1, 1, 1, -1, -1, 1, 1]
	self.vz = [-1, -1, -1, -1, 1, 1, 1, 1]
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.ax = self.ax + 0.05
	  self.ay = self.ay + 0.08
	end

	clear(rgb(0, 0, 6))

	var sx = []
	var sy = []
	var sz = []
	var ca = math.cos(self.ax)
	var sa = math.sin(self.ax)
	var cb = math.cos(self.ay)
	var sb = math.sin(self.ay)
	var cx = width() / 2
	var cy = height() / 2

	for i : 0 .. 7
	  var x = self.vx[i]
	  var y = self.vy[i]
	  var z = self.vz[i]
	  # Rotate about X, then Y.
	  var y1 = y * ca - z * sa
	  var z1 = y * sa + z * ca
	  var x2 = x * cb + z1 * sb
	  var z2 = z1 * cb - x * sb
	  # Perspective, with the vertical squeezed by the panel height.
	  var k = 5.5 / (4.0 + z2)
	  sx.push(cx + int(x2 * k * 4.2))
	  sy.push(cy + int(y1 * k * 3.4))
	  sz.push(z2)
	end

	var edges = [
	  [0, 1], [1, 2], [2, 3], [3, 0],
	  [4, 5], [5, 6], [6, 7], [7, 4],
	  [0, 4], [1, 5], [2, 6], [3, 7]
	]
	var drift = int(now_ms() / 40) % 255

	for e : edges
	  var a = e[0]
	  var b = e[1]
	  var depth = (sz[a] + sz[b]) / 2.0
	  var bright = 150 + int(depth * 90)
	  if bright < 60
		bright = 60
	  end
	  if bright > 255
		bright = 255
	  end
	  line(sx[a], sy[a], sx[b], sy[b], rgb(bright / 4, bright, 255 - drift / 3))
	end

	for i : 0 .. 7
	  pixel(sx[i], sy[i], rgb(255, 255, 255))
	end
  end
end

return App()
