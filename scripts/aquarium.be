# name: Aquarium
# summary: Three fish that glide and turn, swaying weed, light rays, a crab and a chest that opens. Press to feed them.
# author: Stipple
# tags: ambient, animation, button
# panel: 52x16

# Rebuilt for 52x16 from Galadril's Pixel Aquarium, which was written for a
# 32x8 panel. Sixteen rows is a water column, so the extra pixels are spent on
# depth: a lit surface, rays that fade with the water, weed tall enough to sway
# as a wave, and fish at different heights that can pass each other.
#
# Everything that moves keeps a float position and is stepped every 33 ms, so
# fish glide rather than tick a pixel at a time. The sprites are rows of
# characters (see spr) so a new fish is a new picture, not new code.

import math

class App
  var fx, fy, fty, fdir, fnext, fturn, fhold, fhungry
  var ftype, fw, fh, fmax, fspd, foffx, foffy, fbody, ftail
  var bx, by, bph, bpop
  var kx, ky, kl
  var foodX, foodY, feeding
  var crabX, crabDir, crabPause
  var chestT
  var visX, visY, visDir, visOn
  var last, tk, tp, nt
  var tang, tiny, angel, shark, spots

  def init()
	# 0 tang, 1 tiny (a school of four, led by fish 3), 2 angelfish.
	self.ftype = [0, 0, 2, 1, 1, 1, 1]
	self.fw = [6, 6, 5, 3, 3, 3, 3]
	self.fh = [3, 3, 5, 1, 1, 1, 1]
	self.fmax = [10, 10, 8, 11, 11, 11, 11]
	self.fx = [6.0, 30.0, 18.0, 40.0, 36.0, 33.0, 35.0]
	self.fy = [3.0, 7.0, 4.0, 5.0, 6.0, 4.0, 7.0]
	self.fty = [3.0, 7.0, 4.0, 5.0, 6.0, 4.0, 7.0]
	self.fdir = [1, -1, 1, -1, -1, -1, -1]
	self.fnext = [1, -1, 1, -1, -1, -1, -1]
	self.fspd = [0.16, 0.12, 0.07, 0.30, 0.30, 0.30, 0.30]
	self.foffx = [0, 0, 0, 0, 4, 7, 5]
	self.foffy = [0, 0, 0, 0, -1, 1, 2]
	self.fturn = [0, 0, 0, 0, 0, 0, 0]
	self.fhold = [0, 0, 0, 0, 0, 0, 0]
	self.fhungry = [false, false, false, false, false, false, false]
	self.fbody = [0xFF7A18, 0x00BFFF, 0xFFD060, 0xB0E0FF, 0xB0E0FF, 0xB0E0FF, 0xB0E0FF]
	self.ftail = [0xFFB000, 0x0066FF, 0xC08020, 0x6090D0, 0x6090D0, 0x6090D0, 0x6090D0]

	self.tang = ["t.bbb.", "tbbbeb", "t.www."]
	self.tiny = ["tbe"]
	self.angel = ["..f..", "tbwb.", "tbbeb", "tbwb.", "..f.."]
	self.shark = ["....b.......", "..bbbbbbbb.t", "bbbbbbbbbbbt", "....bbb....."]
	self.spots = [3, 9, 14, 22, 27, 35, 41, 48]

	# Eight bubbles, started at different heights so they never rise in step.
	self.bx = [9.0, 21.0, 33.0, 44.0, 15.0, 38.0, 5.0, 27.0]
	self.by = [11.0, 8.0, 13.0, 6.0, 3.0, 10.0, 4.0, 12.0]
	self.bph = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 0.5, 1.5]
	self.bpop = [0, 0, 0, 0, 0, 0, 0, 0]

	self.kx = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
	self.ky = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
	self.kl = [0, 0, 0, 0, 0, 0, 0, 0]

	self.foodX = 26.0
	self.foodY = 0.0
	self.feeding = false

	self.crabX = 10.0
	self.crabDir = 1
	self.crabPause = 0
	self.chestT = 0
	self.visX = 0.0
	self.visY = 3
	self.visDir = 1
	self.visOn = false

	self.last = 0
	self.tk = 0
	self.tp = 0.0
	self.nt = false
  end

  def night()
	if !time_known()
	  return false
	end
	var h = hour()
	return h >= 22 || h < 7
  end

  def clamp(v, lo, hi)
	if v < lo
	  return lo
	end
	if v > hi
	  return hi
	end
	return v
  end

  def on_button(name)
	# Food lands somewhere in the middle, away from the glass. Most fish go for
	# it; a few are not hungry, as in a real tank.
	self.foodX = 8.0 + (math.rand() % 36)
	self.foodY = 0.0
	self.feeding = true
	for i : 0 .. 2
	  self.fhungry[i] = (math.rand() % 4) != 0
	end
	self.fhungry[0] = true
  end

  def calm()
	for i : 0 .. 2
	  self.fhungry[i] = false
	end
  end

  def face(i, d)
	# A turn takes a few frames, drawn edge-on, rather than a flip.
	if self.fdir[i] != d && self.fturn[i] == 0
	  self.fturn[i] = 4
	  self.fnext[i] = d
	end
  end

  def crumbs()
	for k : 0 .. 7
	  self.kx[k] = self.foodX + (math.rand() % 5) - 2
	  self.ky[k] = self.foodY
	  self.kl[k] = 40 + math.rand() % 30
	end
  end

  def step()
	self.tk += 1
	var scale = self.nt ? 0.45 : 1.0

	if self.feeding
	  self.foodY += 0.04
	  if self.foodY >= 13.0
		self.feeding = false
		self.calm()
	  end
	end

	var ate = false
	for i : 0 .. 2
	  var w = self.fw[i]
	  var h = self.fh[i]
	  if self.fturn[i] > 0
		self.fturn[i] -= 1
		if self.fturn[i] == 0
		  self.fdir[i] = self.fnext[i]
		end
	  else
		var want = 0
		var mult = 1.0
		var chase = self.feeding && self.fhungry[i]
		if chase
		  self.fhold[i] = 0
		  var mid = self.fx[i] + w / 2.0
		  if mid < self.foodX - 1
			want = 1
		  elif mid > self.foodX + 1
			want = -1
		  end
		  self.fty[i] = self.clamp(self.foodY, 2.0, self.fmax[i] * 1.0)
		  mult = 1.9
		elif i > 3
		  self.fhold[i] = 0
		  var gap = (self.fx[3] - self.fdir[3] * self.foffx[i]) - self.fx[i]
		  if gap > 1
			want = 1
		  elif gap < -1
			want = -1
		  end
		  mult = self.clamp(0.7 + math.abs(gap) * 0.12, 0.7, 2.5)
		  self.fty[i] = self.clamp(self.fy[3] + self.foffy[i], 2.0, self.fmax[i] * 1.0)
		else
		  if self.fhold[i] > 0
			self.fhold[i] -= 1
		  elif (math.rand() % 420) == 0
			self.fhold[i] = 30 + math.rand() % 60
		  elif (math.rand() % 500) == 0
			want = -self.fdir[i]
		  end
		  if (math.rand() % 150) == 0
			self.fty[i] = 2.0 + (math.rand() % (self.fmax[i] - 1))
		  end
		end

		if want != 0 && want != self.fdir[i]
		  self.face(i, want)
		elif self.fhold[i] == 0
		  self.fx[i] += self.fdir[i] * self.fspd[i] * scale * mult
		end

		# Turn at the glass rather than swimming through it.
		if self.fx[i] > width() - w - 1 && self.fdir[i] > 0
		  self.face(i, -1)
		elif self.fx[i] < 1 && self.fdir[i] < 0
		  self.face(i, 1)
		end
	  end

	  self.fy[i] += (self.fty[i] - self.fy[i]) * 0.04

	  if self.feeding && self.fhungry[i]
		var dx = math.abs(self.fx[i] + w / 2.0 - self.foodX)
		var dy = math.abs(self.fy[i] + h / 2.0 - self.foodY)
		if dx <= 2.5 && dy <= 2.0
		  ate = true
		  self.feeding = false
		  self.crumbs()
		end
	  end
	end
	if ate
	  self.calm()
	end

	for k : 0 .. 7
	  if self.kl[k] > 0
		self.kl[k] -= 1
		self.ky[k] += 0.05
		if self.ky[k] >= 13.0
		  self.kl[k] = 0
		end
	  end
	end

	for b : 0 .. 7
	  if self.bpop[b] > 0
		self.bpop[b] -= 1
		if self.bpop[b] == 0
		  # Back to the sand, somewhere new.
		  self.by[b] = 13.0
		  self.bx[b] = 2.0 + (math.rand() % 48)
		end
	  else
		self.by[b] -= self.by[b] < 6 ? 0.16 : 0.1
		self.bph[b] += 0.15
		if self.by[b] < 1.0
		  self.bpop[b] = 3
		end
	  end
	end

	# The chest opens for two seconds every fifteen, and lets out a burst.
	self.chestT = (self.chestT + 1) % 450
	if self.chestT == 8
	  for j : 0 .. 2
		var b = 5 + j
		self.bpop[b] = 0
		self.bx[b] = 25.0 + j * 2
		self.by[b] = 10.0 - j * 1.5
	  end
	end

	if self.crabPause > 0
	  self.crabPause -= 1
	else
	  self.crabX += self.crabDir * 0.05
	  if (math.rand() % 260) == 0
		self.crabPause = 40 + math.rand() % 80
	  elif (math.rand() % 400) == 0
		self.crabDir = -self.crabDir
	  end
	  if self.crabX > 46
		self.crabDir = -1
	  elif self.crabX < 1
		self.crabDir = 1
	  end
	end

	if self.visOn
	  self.visX += self.visDir * 0.18
	  if self.visX < -14 || self.visX > 66
		self.visOn = false
	  end
	elif (math.rand() % 1500) == 0
	  self.visOn = true
	  self.visDir = (math.rand() % 2) == 0 ? 1 : -1
	  self.visX = self.visDir > 0 ? -13.0 : 53.0
	  self.visY = 3 + math.rand() % 3
	end
  end

  # One picture from rows of characters. b body, t tail, f fin, e eye, w pale
  # marking, . nothing. The tail opens and closes with wag, which is the
  # swimming.
  def spr(x, y, rows, right, body, tail, wag)
	var w = size(rows[0])
	var mid = size(rows) / 2
	for r : 0 .. size(rows) - 1
	  var row = rows[r]
	  for c : 0 .. w - 1
		var ch = row[c]
		# The tail closes to its middle pixel on the wag frame, so it stays
		# joined to the body instead of splitting apart.
		if ch == "t" && wag == 1 && r != mid
		  ch = "."
		end
		if ch != "."
		  var col = body
		  var dy = 0
		  if ch == "t"
			col = tail
		  elif ch == "f"
			col = tail
		  elif ch == "e"
			col = self.nt ? 0x8090A0 : 0xFFFFFF
		  elif ch == "w"
			col = self.nt ? 0x304050 : 0xF0F0C0
		  end
		  pixel(right ? x + c : x + (w - 1 - c), y + r + dy, col)
		end
	  end
	end
  end

  def draw_water()
	var a = 0x00284A
	var b = 0x001C3A
	var c = 0x001430
	var d = 0x000C20
	if self.nt
	  a = 0x001020
	  b = 0x000C18
	  c = 0x000814
	  d = 0x000610
	end
	rect_fill(0, 0, 52, 4, a)
	rect_fill(0, 4, 52, 4, b)
	rect_fill(0, 8, 52, 4, c)
	rect_fill(0, 12, 52, 4, d)

	if self.visOn
	  var col = self.nt ? 0x000408 : 0x000E1E
	  self.spr(int(self.visX), self.visY, self.shark, self.visDir > 0, col, col, 0)
	end

	# Rays from the surface, thinning as they go down. At night, one moonbeam.
	var count = self.nt ? 0 : 3
	for k : 0 .. count
	  var x0 = 6 + k * 13 + int(math.sin(self.tp * 0.35 + k * 1.7) * 2.0)
	  if self.nt
		x0 = 38
	  end
	  var c1 = self.nt ? 0x081A2C : 0x00385E
	  var c2 = self.nt ? 0x061424 : 0x002C50
	  for yy : 1 .. 11
		var xx = x0 + yy / 3
		if yy < 6
		  pixel(xx, yy, c1)
		  pixel(xx + 1, yy, c1)
		elif yy % 2 == 0
		  pixel(xx, yy, c2)
		end
	  end
	end
  end

  def draw_surface()
	var lit = self.nt ? 0x0A2A40 : 0x2A7AA8
	var mid = self.nt ? 0x061A2A : 0x0E4468
	for x : 0 .. width() - 1
	  var v = math.sin(x * 0.55 + self.tp * 2.2)
	  if v > 0.35
		pixel(x, 0, lit)
	  elif v > -0.2
		pixel(x, 0, mid)
	  end
	end
  end

  def draw_bed()
	var sand = 0xD6A434
	var dark = 0x8A6325
	var lite = 0xF0C052
	if self.nt
	  sand = 0x3A2B12
	  dark = 0x261C0C
	  lite = 0x4C3A18
	end
	# Two rows, not one. Depth is the point of the taller panel.
	rect_fill(0, 14, width(), 2, sand)
	for i : 0 .. size(self.spots) - 1
	  pixel(self.spots[i], 14, dark)
	  rect_fill(self.spots[i] + 1, 15, 2, 1, lite)
	end
  end

  # A frond swaying from a fixed root. The lean grows with height and runs up
  # it as a wave, rather than the whole plant flipping between two poses.
  def weed(rootX, tall, a, b)
	var ph = self.tp * 1.4 + rootX * 0.4
	for n : 0 .. tall - 1
	  var off = int(math.sin(ph - n * 0.55) * n * 0.28)
	  pixel(rootX + off, 13 - n, n % 2 == 0 ? a : b)
	end
  end

  def draw_plants()
	var a = self.nt ? 0x00301A : 0x00AA44
	var b = self.nt ? 0x004A26 : 0x00DD66
	self.weed(3, 8, a, b)
	self.weed(6, 5, b, a)
	self.weed(17, 4, a, b)
	self.weed(33, 3, b, a)
	self.weed(46, 7, a, b)
	self.weed(49, 5, b, a)
  end

  def draw_scenery()
	var rock = self.nt ? 0x1C1C1C : 0x5A5A5A
	rect_fill(12, 12, 3, 2, rock)
	pixel(13, 11, rock)
	rect_fill(38, 12, 4, 2, rock)
	pixel(39, 11, rock)

	var coral = self.nt ? 0x40182A : 0xE0507A
	rect_fill(20, 10, 1, 4, coral)
	pixel(19, 11, coral)
	pixel(21, 10, coral)
	pixel(19, 10, coral)

	var star = self.nt ? 0x4A2010 : 0xFF6A30
	rect_fill(33, 13, 3, 1, star)
	pixel(34, 12, star)
  end

  def draw_chest()
	var brown = self.nt ? 0x341A08 : 0x8B4513
	var gold = self.nt ? 0x5A3C00 : 0xFFBB00
	if self.chestT < 70
	  # Lid propped open, treasure showing.
	  rect_fill(23, 12, 7, 2, brown)
	  rect_fill(24, 9, 5, 1, brown)
	  rect_fill(24, 11, 5, 1, gold)
	  if !self.nt && self.chestT % 6 < 3
		pixel(26, 10, 0xFFFFFF)
	  end
	else
	  rect_fill(23, 11, 7, 3, brown)
	  rect_fill(23, 11, 7, 1, gold)
	  pixel(26, 12, gold)
	end
  end

  def draw_jelly()
	# Only after dark: a slow glowing jellyfish, pulsing as it drifts.
	var px = int(26 + math.sin(self.tp * 0.13) * 20)
	var py = int(6 + math.sin(self.tp * 0.5) * 3)
	var glow = 0x8040C0
	var tent = 0x402070
	if math.sin(self.tp * 2.5) > 0
	  rect_fill(px, py, 4, 2, glow)
	else
	  rect_fill(px, py, 4, 1, glow)
	  rect_fill(px + 1, py + 1, 2, 1, glow)
	end
	for k : 0 .. 1
	  for j : 1 .. 3
		pixel(px + k * 2 + int(math.sin(self.tp * 2.0 + j + k) * 1.2), py + 1 + j, tent)
	  end
	end
  end

  def draw_fish(i)
	var w = self.fw[i]
	var h = self.fh[i]
	var body = self.fbody[i]
	var tail = self.ftail[i]
	if self.nt
	  body = 0x203040
	  tail = 0x152030
	end
	var x = int(self.fx[i])
	var y = int(self.fy[i] + math.sin(self.tp * 1.7 + i) * 0.6)
	if self.fturn[i] > 0
	  line(x + w / 2, y, x + w / 2, y + h - 1, body)
	  return
	end
	var rate = self.fhold[i] > 0 ? 2.5 : 5.0
	var wag = int(self.tp * rate + i) % 2
	var rows = self.tang
	if self.ftype[i] == 1
	  rows = self.tiny
	  wag = 0
	elif self.ftype[i] == 2
	  rows = self.angel
	end
	self.spr(x, y, rows, self.fdir[i] > 0, body, tail, wag)
  end

  def draw_bubbles()
	var c = self.nt ? 0x224466 : 0x44CCFF
	for b : 0 .. 7
	  var x = int(self.bx[b] + math.sin(self.bph[b]) * 0.9)
	  if self.bpop[b] > 0
		var p = self.bpop[b]
		var pc = rgb(40 + p * 40, 90 + p * 40, 130 + p * 30)
		pixel(x, 0, pc)
		pixel(x - 1, 1, pc)
		pixel(x + 1, 1, pc)
	  else
		var y = int(self.by[b])
		pixel(x, y, c)
		# Bubbles swell as the water gets shallower.
		if self.by[b] < 6
		  pixel(x + 1, y, self.nt ? 0x112233 : 0x2A7AA0)
		end
	  end
	end
  end

  def draw_crab()
	var x = int(self.crabX)
	var body = self.nt ? 0x3A1610 : 0xD03A2A
	var claw = self.nt ? 0x4A2018 : 0xFF5A40
	var wag = self.crabPause > 0 ? 0 : int(self.tp * 6.0) % 2
	rect_fill(x, 12, 4, 1, body)
	pixel(x, 11 - wag, claw)
	pixel(x + 3, 12 - (1 - wag), claw)
	pixel(x + wag, 13, body)
	pixel(x + 3 - wag, 13, body)
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0
	self.nt = self.night()

	self.draw_water()
	self.draw_bed()
	self.draw_plants()
	self.draw_scenery()
	self.draw_chest()
	if self.nt
	  self.draw_jelly()
	end
	for i : 0 .. 2
	  self.draw_fish(i)
	end
	for k : 0 .. 7
	  if self.kl[k] > 0
		pixel(int(self.kx[k]), int(self.ky[k]), 0xB08050)
	  end
	end
	if self.feeding
	  pixel(int(self.foodX + math.sin(self.foodY * 2.0) * 0.9), int(self.foodY), 0xE0A060)
	end
	self.draw_crab()
	self.draw_bubbles()
	self.draw_surface()
  end
end

return App()
