# name: Hootie
# summary: A virtual pet owl with a full life - it hatches, grows, ages, and dies if neglected.
# author: Stipple
# tags: pet, interactive, animation, ambient
# panel: 52x16

import math

class Hootie
  var full, happy, energy   # the three needs, 0..100
  var stage, age, gen       # life stage (0..4), age in seconds, generation
  var hp, care              # health 0..100, lifetime care counter
  var last, ageacc, save    # frame clock, seconds accumulator, save throttle
  var sel, acts             # selected action index and the labels
  var menu, mtime           # one-button menu: open flag + last-press time
  var react, runtil         # reaction: 0 none, 1 fed, 2 played, 3 rested, 4 petted
  var statsmode, stxt, sbase  # STATS scroll overlay
  var deadtime              # now_ms at death, for the grave and auto-respawn

  # Life-stage timings, in seconds of on-screen age.
  var HATCH, GROW, ELDER, OLD

  def init()
    self.HATCH = 60           # egg -> baby
    self.GROW  = 600          # baby -> adult
    self.ELDER = 1800         # adult -> elder
    self.OLD   = 3600         # elder -> death of old age
	self.full   = self._clamp(store.get("owl_f", 80))
	self.happy  = self._clamp(store.get("owl_h", 80))
	self.energy = self._clamp(store.get("owl_e", 80))
	self.stage  = store.get("owl_stage", 0)
	self.age    = store.get("owl_age", 0)
	self.gen    = store.get("owl_gen", 1)
	self.hp     = self._clamp(store.get("owl_hp", 100))
	self.care   = store.get("owl_care", 0)
	self.last = now_ms()
	self.ageacc = 0
	self.save = 0
	self.sel = 0
	self.acts = ["FEED", "PLAY", "REST", "PET", "STATS"]
	self.menu = false
	self.mtime = 0
	self.react = 0
	self.runtil = 0
	self.statsmode = false
	self.stxt = ""
	self.sbase = -1
	self.deadtime = 0
  end

  def _clamp(v)
	if v < 0 return 0 end
	if v > 100 return 100 end
	return v
  end

  def duration()
	return 15000
  end

  def on_button(b)
	if self.stage == 4
	  self._newEgg()
	  return
	end
	if self.stage == 0
	  return
	end
	if self.statsmode
	  self.statsmode = false
	  return
	end
	if !self.menu
	  self.menu = true
	  self.sel = 0
	else
	  self.sel = (self.sel + 1) % size(self.acts)
	end
	self.mtime = now_ms()
  end

  def _do(id)
	if id == 4
	  # STATS shows a scrolling readout rather than changing a need.
	  var names = ["EGG", "BABY", "ADULT", "ELDER", "GONE"]
	  self.stxt = (names[self.stage] + "  GEN " + str(self.gen) +
				   "  AGE " + str(self.age / 60) + "M  HP " + str(self.hp) +
				   "%  CARE " + str(self.care))
	  self.statsmode = true
	  self.sbase = -1
	  return
	end
	if id == 0
	  self.full = self._clamp(self.full + 35)
	  self.react = 1
	elif id == 1
	  # Play cheers the owl up but tires it out - the trade-off that makes the
	  # needs pull against each other instead of all wanting food.
	  self.happy = self._clamp(self.happy + 30)
	  self.energy = self._clamp(self.energy - 15)
	  self.react = 2
	elif id == 2
	  self.energy = self._clamp(self.energy + 40)
	  self.react = 3
	else
	  # Petting is the gentle option: a little happiness for free, no cost.
	  self.happy = self._clamp(self.happy + 15)
	  self.react = 4
	end
	self.care += 1
	self.runtil = now_ms() + 1600
	self._persist()
  end

  def _persist()
	store.set("owl_f", self.full)
	store.set("owl_h", self.happy)
	store.set("owl_e", self.energy)
	store.set("owl_stage", self.stage)
	store.set("owl_age", self.age)
	store.set("owl_gen", self.gen)
	store.set("owl_hp", self.hp)
	store.set("owl_care", self.care)
  end

  # ---------------------------------------------------------------- lifecycle
  def _age(dt)
	self.ageacc += dt

	if self.ageacc > 3600000
	  self.ageacc = 3600000
	end

	var steps = 0
	while self.ageacc >= 1000 && steps < 120
	  self.ageacc -= 1000
	  self._second()
	  steps += 1
	end
  end

  def _second()
	self.age += 1

	if self.stage == 0
	  if self.age >= self.HATCH self._hatch() end
	  return
	end
	if self.stage == 4
	  return
	end

	# Needs drift down on their own cadences.
	if self.age % 9 == 0  self.full   = self._clamp(self.full - 1)   end
	if self.age % 11 == 0 self.happy  = self._clamp(self.happy - 1)  end
	if self.age % 14 == 0 self.energy = self._clamp(self.energy - 1) end

	# Health: each empty need wounds it every second; steady good care heals it.
	var bad = 0
	if self.full == 0 bad += 1 end
	if self.happy == 0 bad += 1 end
	if self.energy == 0 bad += 1 end
	if bad > 0
	  self.hp = self._clamp(self.hp - bad)
	elif self.full > 30 && self.happy > 30 && self.energy > 30
	  self.hp = self._clamp(self.hp + 1)
	end
	if self.hp <= 0
	  self._die()
	  return
	end

	# Growth by age, then eventual old age.
	if self.stage == 1 && self.age >= self.GROW
	  self.stage = 2
	  self._persist()
	elif self.stage == 2 && self.age >= self.ELDER
	  self.stage = 3
	  self._persist()
	elif self.stage == 3 && self.age >= self.OLD
	  self._die()
	end
  end

  def _hatch()
	self.stage = 1
	self.full = 80
	self.happy = 80
	self.energy = 80
	self.hp = 100
	self._persist()
  end

  def _die()
	self.stage = 4
	self.deadtime = now_ms()
	self.menu = false
	self.statsmode = false
	self._persist()
  end

  def _newEgg()
	self.gen += 1
	self.stage = 0
	self.age = 0
	self.full = 80
	self.happy = 80
	self.energy = 80
	self.hp = 100
	self.care = 0
	self.menu = false
	self._persist()
  end

  # ---------------------------------------------------------------- sprites
  def _owl(ox, oy, blink, flap, mood, elder)
	var body  = elder ? rgb(120, 96, 78) : rgb(150, 95, 55)
	var belly = elder ? rgb(210, 195, 175) : rgb(205, 170, 125)
	var dark  = rgb(90, 58, 34)
	var white = rgb(240, 238, 230)
	var beak  = rgb(230, 150, 40)

	# Ear tufts.
	pixel(ox + 2, oy, dark)
	pixel(ox + 9, oy, dark)

	# Rounded body.
	rect_fill(ox + 1, oy + 1, 10, 12, body)
	pixel(ox, oy + 4, body)
	pixel(ox + 11, oy + 4, body)

	# Pale belly.
	rect_fill(ox + 3, oy + 6, 6, 6, belly)

	# Eyes - a closed line when blinking, otherwise big with a pupil that drops
	# when the owl is unhappy.
	var ey = oy + 3
	if blink
	  line(ox + 2, ey + 1, ox + 4, ey + 1, dark)
	  line(ox + 7, ey + 1, ox + 9, ey + 1, dark)
	else
	  rect_fill(ox + 2, ey, 3, 3, white)
	  rect_fill(ox + 7, ey, 3, 3, white)
	  var py = mood < 0 ? ey + 1 : ey + 2
	  pixel(ox + 3, py, dark)
	  pixel(ox + 8, py, dark)
	end

	# An elder gets white brows.
	if elder
	  line(ox + 1, oy + 2, ox + 3, oy + 2, white)
	  line(ox + 7, oy + 2, ox + 9, oy + 2, white)
	end

	# Beak.
	pixel(ox + 5, oy + 6, beak)
	pixel(ox + 6, oy + 6, beak)
	pixel(ox + 5, oy + 7, rgb(200, 120, 30))

	# Wings, raised on the flap frame.
	if flap
	  line(ox, oy + 4, ox, oy + 7, body)
	  line(ox + 11, oy + 4, ox + 11, oy + 7, body)
	else
	  line(ox, oy + 6, ox, oy + 9, body)
	  line(ox + 11, oy + 6, ox + 11, oy + 9, body)
	end

	# Feet.
	pixel(ox + 3, oy + 13, beak)
	pixel(ox + 8, oy + 13, beak)
  end

  # A smaller, tuftless chick for the BABY stage.
  def _owlet(ox, oy, blink)
	var body  = rgb(175, 125, 85)
	var belly = rgb(215, 185, 145)
	var dark  = rgb(90, 58, 34)
	var white = rgb(240, 238, 230)
	var beak  = rgb(230, 150, 40)

	rect_fill(ox + 1, oy + 1, 6, 7, body)
	pixel(ox, oy + 3, body)
	pixel(ox + 7, oy + 3, body)
	rect_fill(ox + 2, oy + 4, 4, 4, belly)

	var ey = oy + 2
	if blink
	  line(ox + 1, ey + 1, ox + 2, ey + 1, dark)
	  line(ox + 5, ey + 1, ox + 6, ey + 1, dark)
	else
	  rect_fill(ox + 1, ey, 2, 2, white)
	  rect_fill(ox + 5, ey, 2, 2, white)
	  pixel(ox + 2, ey + 1, dark)
	  pixel(ox + 5, ey + 1, dark)
	end

	pixel(ox + 3, oy + 4, beak)
	pixel(ox + 4, oy + 4, beak)
	pixel(ox + 3, oy + 8, beak)
	pixel(ox + 5, oy + 8, beak)
  end

  def _egg(ox, oy, crack)
	var sh   = rgb(235, 225, 195)
	var spot = rgb(200, 175, 130)
	rect_fill(ox + 2, oy, 4, 1, sh)
	rect_fill(ox + 1, oy + 1, 6, 1, sh)
	rect_fill(ox, oy + 2, 8, 6, sh)
	rect_fill(ox + 1, oy + 8, 6, 1, sh)
	rect_fill(ox + 2, oy + 9, 4, 1, sh)
	pixel(ox + 2, oy + 3, spot)
	pixel(ox + 5, oy + 5, spot)
	pixel(ox + 3, oy + 7, spot)
	if crack
	  line(ox + 2, oy + 4, ox + 4, oy + 3, rgb(120, 100, 70))
	  line(ox + 4, oy + 3, ox + 5, oy + 5, rgb(120, 100, 70))
	end
  end

  def _grave(ox)
	var stone = rgb(120, 120, 130)
	var dark  = rgb(80, 80, 90)
	line(ox - 1, 13, ox + 9, 13, rgb(40, 60, 40))
	rect_fill(ox + 2, 5, 5, 8, stone)
	rect_fill(ox + 3, 4, 3, 1, stone)
	line(ox + 4, 7, ox + 4, 10, dark)
	line(ox + 3, 8, ox + 5, 8, dark)
  end

  def _heart(hx, hy, c)
	pixel(hx, hy, c)
	pixel(hx + 2, hy, c)
	line(hx - 1, hy + 1, hx + 3, hy + 1, c)
	pixel(hx, hy + 2, c)
	pixel(hx + 1, hy + 2, c)
	pixel(hx + 2, hy + 2, c)
	pixel(hx + 1, hy + 3, c)
  end

  def _bar(x, y, v, c)
	rect_fill(x, y, 14, 1, rgb(28, 28, 28))
	var w = v * 14 / 100
	if w < 1 && v > 0 w = 1 end
	if w > 0
	  if v < 20 && (now_ms() / 300) % 2 == 0
		rect_fill(x, y, w, 1, rgb((c >> 16) & 255, 40, 40))
	  else
		rect_fill(x, y, w, 1, c)
	  end
	end
  end

  # ---------------------------------------------------------------- draw
  def draw()
	var now = now_ms()
	var dt = now - self.last
	if dt < 0 dt = 0 end
	self.last = now
	self._age(dt)

	self.save += dt
	if self.save >= 5000
	  self.save = 0
	  self._persist()
	end

	clear(rgb(6, 8, 14))

	# STATS overlay: scroll the line once, then drop back to the pet.
	if self.statsmode
	  var laps = scroll_text(self.stxt, rgb(210, 210, 220))
	  if self.sbase < 0
		self.sbase = laps
	  elif laps > self.sbase
		self.statsmode = false
	  end
	  return
	end

	if self.stage == 0
	  self._drawEgg(now)
	  return
	end
	if self.stage == 4
	  self._drawDead(now)
	  return
	end
	self._drawAlive(now)
  end

  def _drawEgg(now)
	# A gentle wobble that quickens as hatching nears.
	var togo = self.HATCH - self.age
	if togo < 0 togo = 0 end
	var rate = togo > 10 ? 500 : 150
	var wob = (now / rate) % 3 - 1
	self._egg(3 + wob, 3, self.age > self.HATCH - 15)
	text(18, 1, "EGG", rgb(210, 200, 170))
	text(18, 9, "GEN " + str(self.gen), rgb(90, 94, 104))
  end

  def _drawDead(now)
	self._grave(3)
	text(20, 2, "R.I.P", rgb(150, 150, 160))
	if (now / 500) % 2 == 0
	  text(16, 9, "NEW EGG?", rgb(120, 124, 134))
	end
	# A press lays the next egg; if left alone, one arrives on its own.
	if now - self.deadtime > 12000
	  self._newEgg()
	end
  end

  def _drawAlive(now)
	var mood = self._mood()

	# A blink every few seconds; a wing flap on a different beat so they do not
	# line up into one mechanical pulse.
	var blink = (now / 220) % 18 == 0
	var flap = (now / 500) % 6 < 1 && mood != 2
	if mood == 2
	  blink = (now / 700) % 2 == 0
	end

	# The right sprite for the stage.
	if self.stage == 1
	  self._owlet(2, 4, blink)
	elif self.stage == 3
	  self._owl(1, 2, blink, flap, mood, true)
	else
	  self._owl(1, 2, blink, flap, mood, false)
	end

	# Reaction bubble above the owl for a short while after an action.
	if self.react != 0 && now < self.runtil
	  var bob = (now / 200) % 2
	  if self.react == 1 || self.react == 2 || self.react == 4
		self._heart(16, 1 - bob, rgb(230, 70, 90))
		self._heart(22, 2 - bob, rgb(230, 120, 140))
	  else
		text(16, 1 - bob, "z", rgb(120, 150, 220))
		text(20, 4 - bob, "z", rgb(90, 120, 200))
	  end
	elif mood == 2
	  if (now / 900) % 2 == 0
		text(14, 2, "z", rgb(90, 120, 200))
	  end
	end

	# A blinking warning when health is failing.
	if self.hp < 30 && (now / 400) % 2 == 0
	  text(30, 1, "!", rgb(230, 60, 60))
	end

	# One-button action menu with an auto-commit countdown.
	if self.menu
	  var dwell = now - self.mtime
	  if dwell >= 1600
		self._do(self.sel)
		self.menu = false
	  else
		text(16, 9, self.acts[self.sel], rgb(240, 220, 150))
		pixel(15, 10, rgb(230, 190, 90))
		pixel(15, 11, rgb(230, 190, 90))
		var cw = 14 - dwell * 14 / 1600
		if cw > 0 rect_fill(16, 14, cw, 1, rgb(230, 190, 90)) end
	  end
	else
	  text(16, 9, "PRESS", rgb(66, 70, 80))
	end

	# Three need bars on the right: green fullness, yellow happiness, blue energy.
	self._bar(37, 1, self.full, rgb(70, 200, 90))
	self._bar(37, 6, self.happy, rgb(230, 200, 60))
	self._bar(37, 11, self.energy, rgb(80, 160, 240))
  end

  # Lowest need decides the mood: content, hungry, bored or sleepy.
  def _mood()
	var m = self.full
	var k = 0
	if self.happy < m m = self.happy k = 1 end
	if self.energy < m m = self.energy k = 2 end
	if m > 45 return -1 end
	return k
  end
end

return Hootie()
