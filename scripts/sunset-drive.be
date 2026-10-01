# name: Sunset Drive
# summary: Pseudo-3D coastal racer at dusk. The knob steers, + speeds up, - brakes. Dodge the traffic; three hits and you are done.
# author: Stipple
# tags: game, racer, interactive, landscape
# panel: 52x16

# @input exclusive

import math
import string

# The road is drawn a row at a time from the horizon down: each row is a step
# nearer, so it is wider, and the bend is applied most at the far end so the
# road seems to swing towards you. Traffic is a list of [lane, progress]
# pairs where progress runs from 0 at the horizon to about 1 at the bumper.
class App
  var px, speed, dist, heading
  var cars, lives, flash
  var dead, best, last, bend

  def init()
	self.best = 0
	self._reset()
  end

  def _reset()
	self.px = 0.0
	self.speed = 0.6
	self.dist = 0.0
	self.heading = 0.0
	self.bend = 0.0
	self.cars = []
	self.lives = 3
	self.flash = 0
	self.dead = false
	self.last = nil
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  self.px -= 0.16
	elif name == 'right'
	  self.px += 0.16
	elif name == 'plus'
	  self.speed += 0.1
	  if self.speed > 1.5 self.speed = 1.5 end
	elif name == 'minus'
	  self.speed -= 0.2
	  if self.speed < 0.3 self.speed = 0.3 end
	end
	if self.px < -1.1 self.px = -1.1 end
	if self.px > 1.1 self.px = 1.1 end
  end

  def _tick()
	self.dist += self.speed
	self.bend = math.sin(self.dist / 90.0) * 0.8 + math.sin(self.dist / 37.0) * 0.2
	self.heading += self.bend * self.speed * 0.6
	# Bends shove the car outward, so lifting off matters.
	self.px += self.bend * self.speed * 0.02
	if self.px < -1.3 self.px = -1.3 end
	if self.px > 1.3 self.px = 1.3 end

	var keep = []
	for c : self.cars
	  c[1] += self.speed * 0.02
	  if c[1] > 0.88 && c[1] < 1.02 && math.abs(c[0] - self.px) < 0.4
		self.lives -= 1
		self.flash = 10
		self.speed = 0.4
		tone(120, 200)
		if self.lives <= 0
		  self.dead = true
		  var score = int(self.dist)
		  if score > self.best self.best = score end
		end
	  elif c[1] < 1.15
		keep.push(c)
	  end
	end
	self.cars = keep

	var clear = true
	for c : self.cars
	  if c[1] < 0.2 clear = false end
	end
	if clear && math.rand() % 100 < 6
	  self.cars.push([((math.rand() % 5) - 2) * 0.4, 0.0])
	end
	if self.flash > 0 self.flash -= 1 end
  end

  def _centre(t)
	return 26.0 + self.bend * (1.0 - t) * (1.0 - t) * 26.0
  end

  def _span(row, x0, x1, colour)
	var w = width()
	if x0 < 0 x0 = 0 end
	if x1 > w - 1 x1 = w - 1 end
	if x1 >= x0
	  rect_fill(x0, row, x1 - x0 + 1, 1, colour)
	end
  end

  def _dot(x, y, colour)
	if x >= 0 && x < width() && y >= 0 && y < height()
	  pixel(x, y, colour)
	end
  end

  def draw()
	var w = width()
	var h = height()
	var now = now_ms()
	var hz = 5

	if self.last == nil self.last = now end
	if now - self.last >= 40
	  self.last = now
	  if !self.dead self._tick() end
	end

	# Sky and a low sun.
	for row : 0 .. hz
	  var k = real(row) / hz
	  rect_fill(0, row, w, 1, rgb(int(20 + 235 * k * k), int(8 + 110 * k * k), int(70 + 20 * k - 60 * k * k)))
	end
	for dx : -4 .. 4
	  for dy : -3 .. 0
		if dx * dx + dy * dy * 2 <= 14
		  self._dot(int(26 + dx - self.heading * 4) % 80 - 10, hz + dy, rgb(255, 210, 100))
		end
	  end
	end

	# Mountains slide sideways as the road bends.
	for x : 0 .. w - 1
	  var u = (x + self.heading * 12.0) / 4.5
	  var top = int(hz - 3.5 * (0.5 + 0.5 * math.sin(u) * math.sin(u * 0.37 + 1.0)))
	  if top < hz
		rect_fill(x, top, 1, hz - top, rgb(48, 20, 72))
	  end
	end

	# Road, row by row from the horizon.
	for i : 1 .. h - hz - 1
	  var row = hz + i
	  var t = real(i) / (h - hz - 1)
	  var c = self._centre(t)
	  var hw = 1.5 + 22.0 * t
	  var band = (int(3.0 / t + self.dist * 0.5) % 2)
	  var grass = band == 0 ? rgb(20, 90, 60) : rgb(14, 70, 50)
	  var road = band == 0 ? rgb(60, 58, 74) : rgb(52, 50, 66)
	  var rumble = band == 0 ? rgb(230, 60, 80) : rgb(230, 230, 240)
	  rect_fill(0, row, w, 1, grass)
	  self._span(row, int(c - hw - 1), int(c + hw + 1), rumble)
	  self._span(row, int(c - hw + 1), int(c + hw - 1), road)
	  if band == 0
		self._dot(int(c + 0.5), row, rgb(250, 210, 80))
	  end
	end

	# Traffic, far to near so nearer cars cover farther ones.
	for c : self.cars
	  var t = c[1] * c[1]
	  var i = int(t * (h - hz - 1) + 0.5)
	  if i >= 1
		var row = hz + i
		var cx = int(self._centre(t) + c[0] * (1.5 + 22.0 * t) * 0.8 + 0.5)
		var half = int(t * 3)
		var tall = 1 + int(t * 2)
		rect_fill(cx - half, row - tall + 1, half * 2 + 1, tall, rgb(80, 180, 255))
		self._dot(cx - half, row, rgb(255, 40, 40))
		self._dot(cx + half, row, rgb(255, 40, 40))
	  end
	end

	# The player's car.
	var carX = int(self._centre(1.0) + self.px * 22.0 * 0.8 + 0.5)
	var body = self.flash > 0 && self.flash % 2 == 0 ? rgb(255, 255, 255) : rgb(255, 210, 40)
	rect_fill(carX - 1, h - 2, 3, 1, body)
	rect_fill(carX - 2, h - 1, 5, 1, body)
	self._dot(carX - 2, h - 1, rgb(255, 30, 30))
	self._dot(carX + 2, h - 1, rgb(255, 30, 30))
	self._dot(carX, h - 2, rgb(120, 200, 255))

	if self.dead
	  # 5x7 font, eight characters a line: two columns over a blackout.
	  rect_fill(0, 0, w, h, rgb(0, 0, 0))
	  text(4, 1, 'GAME', rgb(255, 255, 255))
	  text(4, 9, 'OVER', rgb(255, 90, 90))
	  text(30, 1, string.format('%d', int(self.dist)), rgb(255, 220, 160))
	  if (now / 1500) % 2 == 0
		text(30, 9, string.format('B%d', self.best), rgb(255, 200, 120))
	  else
		text(30, 9, 'PUSH', rgb(120, 200, 255))
	  end
	else
	  text(0, 0, string.format('%d', int(self.dist)), rgb(255, 220, 160))
	  for l : 1 .. self.lives
		pixel(w - l * 3, 1, rgb(255, 70, 90))
	  end
	end
  end
end

return App()
