# name: Ant Farm
# summary: A cross-section of soil where ants dig winding tunnels and carry the dirt up to a growing mound. When it gets crowded the colony starts fresh. Press to add an ant.
# author: Stipple
# tags: ambient, nature, animation, button
# panel: 52x16

# The soil is a grid of cells that are either solid or dug. Ants only walk on
# dug cells. At the end of a tunnel an ant may dig one neighbouring cell, but
# only one that touches a single open cell, which is what keeps the tunnels
# thin. A carrying ant heads for the entrance and adds to the mound.

import math

class App
  var cell, mound, dugN
  var ax, ay, acar, an
  var last, tk

  def init()
	self.ax = []
	self.ay = []
	self.acar = []
	self.an = 0
	self.mound = []
	for x : 0 .. 51
	  self.mound.push(0)
	end
	self.cell = []
	self.last = 0
	self.tk = 0
	self.reset()
  end

  def reset()
	self.cell = []
	for i : 0 .. 52 * 16 - 1
	  self.cell.push(false)
	end
	for y : 3 .. 7
	  self.cell[y * 52 + 26] = true
	end
	self.dugN = 5
	for x : 0 .. 51
	  self.mound[x] = 0
	end
	self.ax = []
	self.ay = []
	self.acar = []
	self.an = 0
	for i : 0 .. 5
	  self.add_ant()
	end
  end

  def add_ant()
	if self.an < 10
	  self.ax.push(26)
	  self.ay.push(4 + math.rand() % 3)
	  self.acar.push(false)
	  self.an += 1
	end
  end

  def on_button(name)
	self.add_ant()
  end

  def open(x, y)
	if x < 0 || x > 51 || y < 2 || y > 15
	  return false
	end
	if y == 2
	  return true
	end
	return self.cell[y * 52 + x]
  end

  def diggable(x, y)
	if x < 1 || x > 50 || y < 4 || y > 14
	  return false
	end
	if self.cell[y * 52 + x]
	  return false
	end
	var n = 0
	if self.open(x - 1, y)
	  n += 1
	end
	if self.open(x + 1, y)
	  n += 1
	end
	if self.open(x, y - 1)
	  n += 1
	end
	if self.open(x, y + 1)
	  n += 1
	end
	return n == 1
  end

  def move_ant(i)
	var x = self.ax[i]
	var y = self.ay[i]
	var dx = [1, -1, 0, 0]
	var dy = [0, 0, 1, -1]
	var pick = -1

	if self.acar[i] && math.rand() % 10 < 7
	  var best = 9999
	  for d : 0 .. 3
		var nx = x + dx[d]
		var ny = y + dy[d]
		if self.open(nx, ny)
		  var score = math.abs(nx - 26) + ny * 3
		  if score < best
			best = score
			pick = d
		  end
		end
	  end
	end

	if pick < 0
	  var start = math.rand() % 4
	  for k : 0 .. 3
		var d = (start + k) % 4
		if pick < 0 && self.open(x + dx[d], y + dy[d]) && (y > 2 || d < 2 || x == 26)
		  pick = d
		end
	  end
	end

	if pick >= 0
	  self.ax[i] = x + dx[pick]
	  self.ay[i] = y + dy[pick]
	end

	if self.ay[i] == 2 && self.acar[i]
	  self.acar[i] = false
	  var mx = 26 + (math.rand() % 11) - 5
	  if self.mound[mx] < 2
		self.mound[mx] += 1
	  end
	end

	if !self.acar[i] && self.ay[i] > 2 && self.dugN < 320 && math.rand() % 5 == 0
	  var d = math.rand() % 3
	  var tx = self.ax[i] + dx[d]
	  var ty = self.ay[i] + dy[d]
	  if self.diggable(tx, ty)
		self.cell[ty * 52 + tx] = true
		self.dugN += 1
		self.acar[i] = true
	  end
	end
  end

  def step()
	self.tk += 1
	for i : 0 .. self.an - 1
	  self.move_ant(i)
	end
	if self.dugN >= 320 && self.tk % 150 == 0
	  self.reset()
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 160
	  self.last = t
	  self.step()
	end

	rect_fill(0, 0, 52, 3, 0x6AB4E8)
	rect_fill(0, 3, 52, 13, 0x7A4A28)
	for y : 3 .. 15
	  for x : 0 .. 51
		if self.cell[y * 52 + x]
		  pixel(x, y, 0x2A1A0E)
		elif (x * 7 + y * 13) % 5 == 0
		  pixel(x, y, 0x5E3A1E)
		end
	  end
	end
	rect_fill(0, 3, 52, 1, 0x3A8A30)
	pixel(26, 3, 0x2A1A0E)
	for x : 0 .. 51
	  var m = self.mound[x]
	  if m > 0
		rect_fill(x, 3 - m, 1, m, 0x9A6A3A)
	  end
	end
	rect_fill(0, 15, 52, 1, 0x3A2412)

	for i : 0 .. self.an - 1
	  pixel(self.ax[i], self.ay[i], self.acar[i] ? 0xFFB040 : 0xE04020)
	end
  end
end

return App()
