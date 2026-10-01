# name: Dutch Trains
# summary: NS trains passing by - VIRM, ICM, ICNG, SLT, FLIRT and more, in both directions.
# author: Stipple
# tags: animation, trains, netherlands, ambient
# panel: 52x16

# Originally written for a 32x8 TC001. Ported to the TC002's 52x16 panel by
# driving the spawn/despawn off width() and stretching the track and ballast
# across the full width, then dropping the whole 8-pixel-tall train onto a
# baseline via a vertical offset (OY) so it sits centred with sky above and a
# little ground below. All the car and cab art is unchanged - it draws through
# px()/ln()/rf() wrappers that add OY, so none of the pixel-art y values moved.

class DutchTrains

  var x, t, old, st, tm, tick, seed, n, len, dir
  var OY

  def init()
	# Sit the 8-tall train on a baseline that centres it on the 16-tall panel.
	self.OY = (height() - 8) / 2
	self.seed = now_ms() + 731
	self.old = -1
	self.pick()
  end

  def px(x, y, c)
	pixel(x, self.OY + y, c)
  end

  def ln(x0, y0, x1, y1, c)
	line(x0, self.OY + y0, x1, self.OY + y1, c)
  end

  def rf(x, y, w, h, c)
	rect_fill(x, self.OY + y, w, h, c)
  end

  def rnd(n)
	self.seed = (self.seed * 1103515245 + 12345) % 2147483647
	if self.seed < 0 self.seed = -self.seed end
	return self.seed % n
  end

  def pick()
	self.t = self.rnd(7)
	if self.t == self.old self.t = (self.t + 1) % 7 end
	self.old = self.t

	if self.rnd(2) == 0 self.dir = 1 else self.dir = -1 end

	# total number of cars
	if self.t == 0 || self.t == 1
	  if self.rnd(2) == 0 self.n = 4 else self.n = 6 end
	elif self.t == 2
	  self.n = 3 + self.rnd(2)
	elif self.t == 3
	  if self.rnd(2) == 0 self.n = 5 else self.n = 8 end
	elif self.t == 4
	  self.n = 3 + self.rnd(2)
	elif self.t == 5
	  if self.rnd(2) == 0 self.n = 4 else self.n = 6 end
	else
	  self.n = 3 + self.rnd(2)
	end

	# every rendered car = 10 px
	self.len = self.n * 10
	self.st = 0
	self.tick = now_ms()

	# Spawn just off the panel on the side the train enters from.
	if self.dir == 1
	  self.x = width()
	else
	  self.x = -self.len
	end
  end

  def wheel(x)
	var c = 0x555555
	if (now_ms() / 120) % 2 == 0 c = 0x999999 end
	self.px(x + 2, 6, c)
	self.px(x + 7, 6, c)
  end

  def car(x)

	# VIRM / DDZ
	if self.t < 2

	  self.rf(x, 2, 10, 4, 0xFFD500)

	  if self.t == 0
		self.ln(x, 2, x + 9, 2, 0x0866A5)
	  else
		self.ln(x, 4, x + 9, 4, 0x0866A5)
	  end

	  self.px(x + 1, 3, 0x163850)
	  self.px(x + 3, 3, 0x163850)
	  self.px(x + 5, 3, 0x163850)
	  self.px(x + 7, 3, 0x163850)
	  self.px(x + 9, 3, 0x163850)

	  self.px(x + 1, 5, 0x163850)
	  self.px(x + 3, 5, 0x163850)
	  self.px(x + 7, 5, 0x163850)
	  self.px(x + 9, 5, 0x163850)

	  self.px(x + 5, 4, 0x0866A5)

	# ICM / ICNG
	elif self.t < 4

	  self.rf(x, 3, 10, 3, 0xFFD500)

	  self.ln(x, 3, x + 9, 3, 0x0866A5)

	  self.px(x + 1, 4, 0x17394F)
	  self.px(x + 3, 4, 0x17394F)
	  self.px(x + 5, 4, 0x17394F)
	  self.px(x + 7, 4, 0x17394F)
	  self.px(x + 9, 4, 0x17394F)

	  self.px(x + 5, 5, 0x0866A5)

	# SPRINTER
	else

	  self.rf(x, 3, 10, 3, 0xEEEEEE)
	  self.ln(x, 3, x + 9, 3, 0x0870A8)

	  self.px(x + 1, 4, 0x17394F)
	  self.px(x + 3, 4, 0x17394F)
	  self.px(x + 5, 4, 0x17394F)
	  self.px(x + 7, 4, 0x17394F)
	  self.px(x + 9, 4, 0x17394F)

	  # yellow doors
	  if self.t == 6
		self.px(x + 4, 4, 0xFFD500)
		self.px(x + 4, 5, 0xFFD500)
	  else
		self.px(x + 5, 4, 0xFFD500)
		self.px(x + 5, 5, 0xFFD500)
	  end

	end

	self.wheel(x)
  end

  def cab(x, left, lead)

	var dbl = self.t < 2
	var spr = self.t > 3
	var body = 0xFFD500

	if spr body = 0xEEEEEE end

	# DOUBLE DECKER
	if dbl

	  self.rf(x + 1, 2, 9, 4, body)

	  if left

		self.px(x, 3, body)
		self.px(x, 4, body)
		self.px(x, 5, body)

		self.px(x + 1, 2, 0x163040)
		self.px(x + 2, 2, 0x163040)
		self.px(x + 1, 3, 0x163040)

		if lead
		  self.px(x, 5, 0xFFFFFF)
		else
		  self.px(x, 5, 0xFF2020)
		end

	  else

		self.px(x + 9, 3, body)
		self.px(x + 9, 4, body)
		self.px(x + 9, 5, body)

		self.px(x + 7, 2, 0x163040)
		self.px(x + 8, 2, 0x163040)
		self.px(x + 8, 3, 0x163040)

		if lead
		  self.px(x + 9, 5, 0xFFFFFF)
		else
		  self.px(x + 9, 5, 0xFF2020)
		end

	  end

	  if self.t == 0
		self.ln(x + 3, 2, x + 8, 2, 0x0866A5)
	  else
		self.ln(x + 3, 4, x + 8, 4, 0x0866A5)
	  end

	  self.px(x + 4, 3, 0x17394F)
	  self.px(x + 6, 3, 0x17394F)
	  self.px(x + 8, 3, 0x17394F)

	  self.px(x + 4, 5, 0x17394F)
	  self.px(x + 6, 5, 0x17394F)
	  self.px(x + 8, 5, 0x17394F)

	# SINGLE DECK
	else

	  self.rf(x + 2, 3, 8, 3, body)

	  if left

		self.px(x + 1, 4, body)
		self.px(x, 5, body)
		self.px(x + 1, 5, body)

		self.px(x + 2, 3, 0x153344)
		self.px(x + 3, 3, 0x153344)
		self.px(x + 1, 4, 0x153344)

		if lead
		  self.px(x, 5, 0xFFFFFF)
		else
		  self.px(x, 5, 0xFF2020)
		end

	  else

		self.px(x + 8, 4, body)
		self.px(x + 9, 5, body)
		self.px(x + 8, 5, body)

		self.px(x + 6, 3, 0x153344)
		self.px(x + 7, 3, 0x153344)
		self.px(x + 8, 4, 0x153344)

		if lead
		  self.px(x + 9, 5, 0xFFFFFF)
		else
		  self.px(x + 9, 5, 0xFF2020)
		end

	  end

	  # ICM
	  if self.t == 2

		self.px(x + 4, 4, 0x17394F)
		self.px(x + 6, 4, 0x17394F)
		self.px(x + 8, 4, 0x17394F)

	  # ICNG
	  elif self.t == 3

		self.ln(x + 4, 3, x + 8, 3, 0x0866A5)
		self.px(x + 4, 4, 0x17394F)
		self.px(x + 6, 4, 0x17394F)
		self.px(x + 8, 4, 0x17394F)

	  # SPRINTERS
	  else

		self.ln(x + 4, 3, x + 8, 3, 0x0870A8)

		self.px(x + 4, 4, 0x17394F)
		self.px(x + 6, 4, 0x17394F)
		self.px(x + 8, 4, 0x17394F)

		# yellow nose
		if left
		  self.px(x + 1, 4, 0xFFD500)
		else
		  self.px(x + 8, 4, 0xFFD500)
		end

	  end

	end

	self.wheel(x)
  end

  def train()

	var p = self.x
	var leadleft = self.dir == 1

	# left end
	self.cab(p, true, leadleft)
	p += 10

	# middle cars
	for i : 1 .. self.n - 2
	  self.car(p)
	  p += 10
	end

	# right end
	self.cab(p, false, !leadleft)

  end

  def world()

	clear(0x00101A)

	var w = width()

	# Faint sky border and the rail, both now spanning the full panel width.
	self.ln(0, 0, w - 1, 0, 0x303030)
	self.ln(0, 7, w - 1, 7, 0x888888)

	# Ballast sleepers, drifting with the train, across the whole width.
	var o = self.x % 4
	var i = 0
	while i * 4 + o < w
	  self.px(i * 4 + o, 7, 0xBBBBBB)
	  i += 1
	end

  end

  def draw()

	var now = now_ms()

	self.world()

	# train passing
	if self.st == 0

	  if now - self.tick > 90

		self.tick = now

		if self.dir == 1
		  self.x -= 1
		else
		  self.x += 1
		end

	  end

	  self.train()

	  if self.dir == 1

		if self.x < -self.len
		  self.st = 1
		  self.tm = now
		end

	  else

		if self.x > width()
		  self.st = 1
		  self.tm = now
		end

	  end

	  return
	end

	# empty track
	if now - self.tm > 2500
	  self.pick()
	end

  end

end

return DutchTrains()
