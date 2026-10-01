# name: Time Glass
# summary: A living pixel landscape that flows through the day and periodically reveals the current time.
# author: Stipple
# tags: clock, ambient, animation, time, button
# panel: 52x16

import math
import string

class App
  var last
  var phase
  var flash

  def draw()
    if !time_known()
      clear(rgb(8, 8, 12))
      text(8, 5, "NO TIME", rgb(180, 80, 80))
      return
    end

    var t = now_ms()

    if self.last == nil
      self.last = t
      self.phase = 0
      self.flash = 0
    end

    if t - self.last >= 80
      self.last = t
      self.phase = self.phase + 1
    end

    clear(rgb(3, 5, 12))

    # --- sky ---
    var h = hour()
    var m = minute()
    var s = second()

    var night = 0
    if h < 7
      night = 1
    end
    if h >= 21
      night = 1
    end

    # stars
    if night == 1
      pixel(3, 2, rgb(90, 110, 150))
      pixel(11, 4, rgb(120, 130, 170))
      pixel(20, 1, rgb(100, 120, 170))
      pixel(29, 3, rgb(150, 150, 190))
      pixel(39, 1, rgb(100, 120, 170))
      pixel(47, 4, rgb(130, 140, 180))
      pixel(50, 2, rgb(90, 110, 150))
    else
      # sun position follows the day
      var sunx = 3 + ((h * 60 + m) * 46 / 840)
      if sunx > 49
        sunx = 49
      end
      pixel(sunx, 2, rgb(255, 220, 80))
      pixel(sunx - 1, 2, rgb(255, 190, 50))
      pixel(sunx + 1, 2, rgb(255, 190, 50))
      pixel(sunx, 1, rgb(255, 240, 120))
    end

    # --- distant moving hills ---
    var p = self.phase
    var x = 0

    while x < 52
      var wave = math.sin((x + p * 0.12) * 0.22)
      var wave2 = math.sin((x - p * 0.07) * 0.11)

      var y = 11 + wave * 2 + wave2 * 1.5
      y = math.floor(y)

      line(x, y, x, 15, rgb(8, 20, 28))
      x = x + 1
    end

    # --- foreground landscape ---
    x = 0
    while x < 52
      var wave = math.sin((x + p * 0.08) * 0.18)
      var y = 13 + math.floor(wave * 1.5)

      line(x, y, x, 15, rgb(12, 35, 38))
      x = x + 1
    end

    # --- flowing horizon lights ---
    var glow = p % 20

    pixel(glow, 12, rgb(30, 100, 100))
    pixel(glow + 1, 12, rgb(20, 70, 80))
    pixel(51 - glow, 11, rgb(20, 70, 80))

    # --- tiny moving fireflies ---
    var f1 = (p * 3) % 50
    var f2 = (p * 2 + 17) % 48
    var f3 = (p * 4 + 31) % 46

    if night == 1
      pixel(f1 + 1, 10, rgb(120, 100, 45))
      pixel(f2 + 2, 8, rgb(100, 100, 40))
      pixel(f3 + 3, 11, rgb(130, 110, 50))
    end

    var cycle = (p / 150) % 12

    if self.flash == 1
      cycle = 9
    end

    if cycle >= 8
      clear(rgb(4, 7, 14))

      # faint horizon remains behind the clock
      line(0, 14, 51, 14, rgb(10, 35, 42))
      line(0, 15, 51, 15, rgb(6, 20, 27))

      var clock = string.format("%02d:%02d", h, m)
      var tw = text_width(clock)
      var tx = (52 - tw) / 2

      # subtle seconds pulse
      var c = rgb(210, 235, 255)
      if s % 2 == 0
        c = rgb(255, 235, 150)
      end

      text(tx, 4, clock, c)

      # tiny progress line through the minute
      var pw = math.floor(m * 50 / 59)
      if pw > 50
        pw = 50
      end

      line(1, 13, pw, 13, rgb(35, 110, 120))
    end

    # reset forced reveal after a while
    if self.flash == 1
      if t - self.last >= 2500
        self.flash = 0
      end
    end
  end

  def on_button(name)
    self.flash = 1
    self.last = now_ms()
  end
end

return App()
