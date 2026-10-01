# name: Blob Pet
# summary: A small slime that wanders, dozes, gets hungry and hops after food. Press to drop it a snack.
# author: Stipple
# tags: ambient, animation, button, game
# panel: 52x16

# The blob is a dome drawn row by row, so squashing it is only a change to its
# width and height. It has five moods: idle, wandering, asleep, chasing food
# and eating. Hunger creeps up on its own; past a threshold a "!" shows and
# it goes looking. Food from the button always gets eaten, hungry or not.

import math

class App
  var last, tk, tp
  var x, dir, state, st, tx, hop
  var hunger, foodOn, foodX, foodY, hearts

  def init()
	self.x = 26.0
	self.dir = 1
	self.state = 0
	self.st = 60
	self.tx = 26.0
	self.hop = 0.0
	self.hunger = 0
	self.foodOn = false
	self.foodX = 0.0
	self.foodY = 0.0
	self.hearts = 0
	self.last = 0
	self.tk = 0
	self.tp = 0.0
  end

  def on_button(name)
	if !self.foodOn
	  self.foodOn = true
	  self.foodX = 6.0 + (math.rand() % 40)
	  self.foodY = 0.0
	  if self.state == 2
		self.state = 0
		self.st = 10
	  end
	end
  end

  def walk_to(tx)
	var d = tx - self.x
	if d > 0.4
	  self.dir = 1
	  self.x += 0.25
	  self.hop += 0.35
	  return false
	elif d < -0.4
	  self.dir = -1
	  self.x -= 0.25
	  self.hop += 0.35
	  return false
	end
	return true
  end

  def step()
	self.tk += 1
	self.hunger += 1
	if self.hearts > 0
	  self.hearts -= 1
	end

	if self.foodOn && self.foodY < 12.0
	  self.foodY += 0.15
	end

	if self.foodOn && self.foodY >= 12.0 && self.state != 4
	  self.state = 3
	end

	if self.state == 3
	  if self.walk_to(self.foodX)
		self.state = 4
		self.st = 20
	  end
	elif self.state == 4
	  self.st -= 1
	  if self.st <= 0
		self.foodOn = false
		self.hunger = 0
		self.hearts = 45
		self.state = 0
		self.st = 50
	  end
	elif self.state == 1
	  if self.walk_to(self.tx)
		self.state = 0
		self.st = 40 + math.rand() % 80
	  end
	elif self.state == 2
	  self.st -= 1
	  if self.st <= 0
		self.state = 0
		self.st = 30
	  end
	else
	  self.st -= 1
	  if self.st <= 0
		var r = math.rand() % 10
		if r < 5
		  self.state = 1
		  self.tx = 6.0 + (math.rand() % 40)
		elif r < 7 && self.hunger < 2700
		  self.state = 2
		  self.st = 200 + math.rand() % 200
		else
		  self.st = 40 + math.rand() % 60
		end
	  end
	end
  end

  def draw_blob()
	var w = 10
	var h = 6
	var breathe = math.sin(self.tp * 3.0)
	if self.state == 2
	  h = 5
	  w = 11
	elif self.state == 1 || self.state == 3
	  var hp = math.abs(math.sin(self.hop))
	  h = 6 + int(hp * 2)
	  w = 10 - int(hp * 2)
	elif self.state == 4
	  h = 5 + (self.tk / 3) % 2
	else
	  h = 6 + (breathe > 0.3 ? 1 : 0)
	end
	var lift = 0
	if self.state == 1 || self.state == 3
	  lift = int(math.abs(math.sin(self.hop)) * 3)
	end
	var base = 14 - lift
	var cx = int(self.x)
	var body = self.state == 2 ? 0x3A9A7A : 0x50E0A0
	var edge = 0x2A9A6A
	for r : 0 .. h - 1
	  var u = r * 1.0 / h
	  var hw = int(w / 2.0 * math.sqrt(1.0 - u * u) + 0.5)
	  rect_fill(cx - hw, base - r, hw * 2 + 1, 1, body)
	  pixel(cx - hw, base - r, edge)
	  pixel(cx + hw, base - r, edge)
	end
	rect_fill(cx - w / 2, base + 1, w + 1, 1, 0x1C6A4A)
	pixel(cx - 2 + self.dir, base - h + 3, 0xFFFFFF)
	pixel(cx + 1 + self.dir, base - h + 3, 0xFFFFFF)
	if self.state == 2
	  pixel(cx - 2, base - h + 3, 0x1C6A4A)
	  pixel(cx + 1, base - h + 3, 0x1C6A4A)
	  var zy = base - h - 1 - int(self.tp * 2.0) % 3
	  text(cx + 4, zy - 5, "z", 0x8090C0)
	else
	  var blink = (self.tk / 4) % 40 == 0
	  if !blink
		pixel(cx - 2 + self.dir, base - h + 3, 0x102030)
		pixel(cx + 1 + self.dir, base - h + 3, 0x102030)
	  end
	end
	if self.state == 4
	  pixel(cx, base - h + 1, 0xFF8080)
	end
	if self.hunger >= 2700 && self.state != 2 && self.state != 4 && (self.tk / 8) % 2 == 0
	  text(cx - 2, base - h - 8, "!", 0xFF5050)
	end
	if self.hearts > 0
	  var hy = base - h - 2 - (45 - self.hearts) / 10
	  pixel(cx - 1, hy, 0xFF5080)
	  pixel(cx + 1, hy, 0xFF5080)
	  pixel(cx, hy + 1, 0xFF5080)
	  pixel(cx, hy, 0xFF5080)
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0

	for y : 0 .. 12
	  rect_fill(0, y, 52, 1, y < 6 ? 0x14203A : 0x1A2A48)
	end
	rect_fill(0, 15, 52, 1, 0x3A2A1A)
	rect_fill(0, 14, 52, 1, 0x4A3822)
	for i : 0 .. 12
	  pixel(i * 4 + 1, 15, 0x2A1E12)
	end

	if self.foodOn
	  var fy = int(self.foodY)
	  pixel(int(self.foodX), fy, 0xFF3030)
	  pixel(int(self.foodX), fy - 1, 0x40B040)
	end
	self.draw_blob()
  end
end

return App()
