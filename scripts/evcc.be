# name: EVCC Energy
# summary: Live energy balance from EVCC - solar, house, car, battery and grid as one picture, with detail on the button.
# author: Stipple
# tags: mqtt, energy, solar, car, home
# panel: 52x16

# @config lp number "Loadpoint" default=1 min=1 max=8 help="Which EVCC loadpoint to follow. Save the script again after changing this - see the note below."
# @config binv boolean "Battery sign is inverted" default=false help="Tick if charging and discharging show the wrong way round. Some EVCC versions report the opposite sign."

import string

class App
  var CPV, CBAT, CIMP, CEXP, CHOME, CCAR, CDIM, CGAP
  var SUNC, SUNR, PYLT, PYLA, PYLP
  var HSER, HSEW, HSEL, CARW, CARB, CART, BATS
  var TOPICS
  var binv, lpok, view
  var STALE

  def init()
    var lp = str(store.get("lp", 1))
    self.binv = store.get("binv", false)

    # Index order is used everywhere below: 0 pv, 1 grid, 2 home,
    # 3 battery soc, 4 battery power, 5 car.
    self.TOPICS = [
      "evcc/site/pvPower",
      "evcc/site/gridPower",
      "evcc/site/homePower",
      "evcc/site/batterySoc",
      "evcc/site/batteryPower",
      "evcc/loadpoints/" + lp + "/chargePower"
    ]

    self.CPV = rgb(232, 163, 63)
    self.CBAT = rgb(53, 181, 201)
    self.CIMP = rgb(217, 69, 95)
    self.CEXP = rgb(79, 201, 111)
    self.CHOME = rgb(180, 180, 180)
    self.CCAR = rgb(155, 127, 224)
    self.CDIM = rgb(80, 80, 88)
    self.CGAP = rgb(14, 14, 18)

    # Two minutes. EVCC publishes every few seconds, so anything this old is
    # a broker holding a retained message for a service that has stopped.
    self.STALE = 120000

    # The original's glyphs, kept as they were - (x, y) pairs, five rows tall.
    self.SUNC = [2,1, 3,1, 4,1, 2,2, 3,2, 4,2, 2,3, 3,3, 4,3]
    self.SUNR = [3,0, 0,2, 6,2, 3,4, 1,0, 5,0, 1,4, 5,4]
    self.PYLT = [3,0, 3,1, 3,2, 3,3, 2,4, 3,4, 4,4]
    self.PYLA = [1,1, 2,1, 4,1, 5,1, 1,3, 2,3, 4,3, 5,3]
    self.PYLP = [0,1, 6,1, 0,3, 6,3]
    self.HSER = [3,0, 2,1, 3,1, 4,1, 1,2, 2,2, 3,2, 4,2, 5,2]
    self.HSEW = [1,3, 2,3, 4,3, 5,3, 1,4, 2,4, 4,4, 5,4]
    self.HSEL = [3,3, 3,4]
    self.CARW = [3,0, 4,0, 5,0]
    self.CARB = [2,1, 3,1, 4,1, 5,1, 6,1, 1,2, 2,2, 3,2, 4,2, 5,2, 6,2, 7,2,
                 0,3, 1,3, 2,3, 3,3, 4,3, 5,3, 6,3, 7,3]
    self.CART = [1,4, 6,4]
    self.BATS = [0,0, 1,0, 2,0, 3,0, 4,0, 5,0, 6,0, 0,1, 6,1, 0,2, 6,2, 7,2,
                 0,3, 6,3, 0,4, 1,4, 2,4, 3,4, 4,4, 5,4, 6,4]

    self.lpok = true
    self.view = 0
  end

  def duration()
    return 18000
  end

  # The only thing that changes the view. Six steps: balance, solar, grid,
  # house, car, battery, and round again.
  def on_button(name)
    if name != "select"
      return
    end
    self.view = (self.view + 1) % 6
  end

  # --- reading -------------------------------------------------------------

  def _v(i)
    var raw = mqtt_get(self.TOPICS[i])
    if raw == nil
      return nil
    end
    return int(real(raw))
  end

  def _or0(v)
    if v == nil
      return 0
    end
    return v
  end

  def _age()
    var best = -1
    var i = 0
    while i < size(self.TOPICS)
      var a = mqtt_age_ms(self.TOPICS[i])
      if a >= 0
        if best < 0 || a < best
          best = a
        end
      end
      i += 1
    end
    return best
  end

  # --- formatting ----------------------------------------------------------

  def _w(v)
    if v == nil
      return "--"
    end
    var a = v
    if a < 0
      a = 0 - a
    end
    if a >= 10000
      return str(int(v / 1000)) + "k"
    end
    if a >= 1000
      return string.format("%d.%dk", int(v / 1000), int((a % 1000) / 100))
    end
    return str(v) + "W"
  end

  def _glyph(g, ox, oy, c)
    var i = 0
    while i < size(g)
      pixel(ox + g[i], oy + g[i + 1], c)
      i += 2
    end
  end

  # --- the balance ---------------------------------------------------------

  def _stack(y, h, vals, cols)
    var w = width()
    var total = 0
    var i = 0
    while i < size(vals)
      if vals[i] > 0
        total += vals[i]
      end
      i += 1
    end

    if total <= 0
      rect_fill(0, y, w, h, self.CGAP)
      return
    end

    var x = 0
    var last = self.CGAP
    i = 0
    while i < size(vals)
      var v = vals[i]
      if v > 0
        var seg = int(v * w / total)
        if seg < 1
          seg = 1
        end
        if x + seg > w
          seg = w - x
        end
        if seg > 0
          rect_fill(x, y, seg, h, cols[i])
          last = cols[i]
          x += seg
        end
      end
      i += 1
    end

    # Rounding leaves a pixel or two; give them to whoever was last rather
    # than leaving a gap that reads as a missing contributor.
    if x < w
      rect_fill(x, y, w - x, h, last)
    end
  end

  def _balance()
    var pv = self._v(0)
    var grid = self._v(1)
    var home = self._v(2)
    var soc = self._v(3)
    var car = self._v(5)

    var b = self._or0(self._v(4))
    if self.binv
      b = 0 - b
    end
    var bdis = 0
    var bchg = 0
    if b > 0
      bdis = b
    else
      bchg = 0 - b
    end

    var g = self._or0(grid)
    var imp = 0
    var exp = 0
    if g > 0
      imp = g
    else
      exp = 0 - g
    end

    var pvw = self._or0(pv)
    if pvw < 0
      pvw = 0
    end

    var ps = self._w(pv)
    text(0, 0, ps, self.CPV)

    var gc = self.CEXP
    if g > 0
      gc = self.CIMP
    end
    var gs = self._w(grid)
    if grid != nil
      gs = self._w(imp + exp)
    end
    text(width() - text_width(gs), 0, gs, gc)

    # Producing, then consuming. The two bars are the same width and the same
    # total, so their segments line up as a real balance.
    self._stack(8, 3, [pvw, bdis, imp], [self.CPV, self.CBAT, self.CIMP])
    self._stack(12, 3, [self._or0(home), self._or0(car), bchg, exp],
                [self.CHOME, self.CCAR, self.CBAT, self.CEXP])

    # The house battery, if there is one. Drawn only when a level has
    # actually arrived - an empty row is honest, a zero-width bar is not.
    if soc != nil
      var lv = soc
      if lv < 0
        lv = 0
      end
      if lv > 100
        lv = 100
      end
      rect_fill(0, 15, width(), 1, self.CGAP)
      var bw = int(lv * width() / 100)
      if bw > 0
        rect_fill(0, 15, bw, 1, self.CBAT)
      end
    end
  end

  # --- detail --------------------------------------------------------------

  def _batt(ox, oy, level)
    self._glyph(self.BATS, ox, oy, rgb(138, 138, 138))
    var lv = level
    if lv < 0
      lv = 0
    end
    if lv > 100
      lv = 100
    end
    var c = self.CEXP
    if lv < 50
      c = self.CPV
    end
    if lv < 20
      c = self.CIMP
    end

    var n = int((lv * 5 + 50) / 100)
    var i = 0
    while i < n
      rect_fill(ox + 1 + i, oy + 1, 1, 3, c)
      i += 1
    end
  end

  def _detail()
    var v = self.view
    var label = ""
    var value = ""
    var colour = self.CHOME

    if v == 1
      label = "SOLAR"
      value = self._w(self._v(0))
      colour = self.CPV
      self._glyph(self.SUNR, 0, 9, self.CPV)
      self._glyph(self.SUNC, 0, 9, rgb(255, 224, 138))
    elif v == 2
      var g = self._or0(self._v(1))
      label = "GRID OUT"
      colour = self.CEXP
      if g > 0
        label = "GRID IN"
        colour = self.CIMP
      end
      var a = g
      if a < 0
        a = 0 - a
      end
      value = self._w(a)
      if self._v(1) == nil
        value = "--"
      end
      self._glyph(self.PYLA, 0, 9, rgb(107, 107, 107))
      self._glyph(self.PYLT, 0, 9, rgb(138, 138, 138))
      self._glyph(self.PYLP, 0, 9, colour)
    elif v == 3
      label = "HOUSE"
      value = self._w(self._v(2))
      colour = self.CHOME
      self._glyph(self.HSEW, 0, 9, self.CHOME)
      self._glyph(self.HSER, 0, 9, rgb(138, 127, 107))
      self._glyph(self.HSEL, 0, 9, rgb(232, 195, 63))
    elif v == 4
      label = "CAR"
      colour = self.CCAR
      value = self._w(self._v(5))
      if !self.lpok
        # The watch was refused, so this would read as a car that is never
        # charging. Naming it beats drawing a plausible nothing.
        label = "CAR"
        value = "re-save"
        colour = self.CIMP
      end
      self._glyph(self.CARB, 0, 9, self.CCAR)
      self._glyph(self.CARW, 0, 9, rgb(217, 204, 245))
      self._glyph(self.CART, 0, 9, rgb(74, 68, 88))
    else
      var soc = self._v(3)
      label = "BATTERY"
      colour = self.CBAT
      if soc == nil
        value = "--"
        self._batt(0, 9, 0)
      else
        value = str(soc) + "%"
        self._batt(0, 9, soc)
      end
    end

    text(0, 0, label, self.CDIM)
    text(10, 9, value, colour)
  end

  # --- frame ---------------------------------------------------------------

  def draw()
    clear(rgb(0, 0, 0))

    # Every frame, because there is nowhere else to ask. Asking again is free
    # and does not consume a second slot.
    var i = 0
    while i < 5
      mqtt_watch(self.TOPICS[i])
      i += 1
    end
    # Kept, because a refused watch is the difference between "the car is not
    # charging" and "this script is not being told about the car".
    self.lpok = mqtt_watch(self.TOPICS[5])

    if !mqtt_known()
      # Not an error. MQTT is off by default, and saying so beats a panel of
      # zeroes that look like a house using no power at all.
      text(2, 0, "no", self.CDIM)
      text(2, 9, "broker", rgb(120, 60, 60))
      return
    end

    var age = self._age()
    if age < 0
      text(1, 0, "waiting", self.CDIM)
      text(1, 9, "for evcc", rgb(70, 70, 78))
      return
    end

    if self.view == 0
      self._balance()
    else
      self._detail()
    end

    if age > self.STALE
      pixel(width() - 1, 7, self.CIMP)
    end
  end
end

return App()
