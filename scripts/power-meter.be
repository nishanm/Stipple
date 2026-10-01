# name: Power Meter
# summary: Live house load from MQTT, with an hour of history and a grey-out when the data goes stale.
# author: Stipple
# tags: mqtt, home, graph, tool
# panel: 52x16

import string

class App
  var TOPIC
  var hist          # 52 samples, one per minute, -1 for "no reading"
  var head          # next slot to write
  var last          # when we last took a sample
  var peak          # top of the graph's scale, watts

  def init()
    self.TOPIC = "home/power/now"
    self.hist = []
    var i = 0
    while i < 52
      self.hist.push(-1)
      i += 1
    end
    self.head = 0
    self.last = -60000
    self.peak = 500
  end

  # Watts, or nil when there is nothing to believe.
  def _watts()
    var raw = mqtt_get(self.TOPIC)
    if raw == nil
      return nil
    end
    # An hour is generous for a meter and short enough that a dead sensor
    # greys out within one carousel rotation of somebody noticing.
    if mqtt_age_ms(self.TOPIC) > 3600000
      return nil
    end
    return int(real(raw))
  end

  def draw()
    clear(rgb(0, 0, 0))
    mqtt_watch(self.TOPIC)

    if !mqtt_known()
      # Not an error. MQTT is off by default, and a panel saying so is more
      # use than one showing a number it made up.
      text(2, 1, "no", rgb(120, 120, 120))
      text(2, 9, "broker", rgb(120, 60, 60))
      return
    end

    var w = self._watts()
    var now = now_ms()

    if w == nil
      text(2, 1, "waiting", rgb(90, 90, 90))
      self._graph()
      return
    end

    # One sample a minute. 52 of them is an hour, which is exactly as much
    # history as a panel 52 pixels wide can hold honestly.
    if now - self.last >= 60000
      self.last = now
      self.hist[self.head] = w
      self.head = (self.head + 1) % 52
    end

    if w > self.peak
      self.peak = w
    end

    var colour = rgb(0, 210, 120)
    if w >= 3000
      colour = rgb(255, 60, 40)
    elif w >= 1000
      colour = rgb(255, 176, 0)
    end

    var label = str(w)
    if w >= 1000
      # "3.4k" beats "3421" on a panel this size: four characters either
      # way, and one of them can be read at a glance from a doorway.
      label = string.format("%d.%dk", w / 1000, (w % 1000) / 100)
    end
    text(1, 0, label, colour)
    text(1 + text_width(label) + 2, 0, "W", rgb(70, 70, 70))

    self._graph()
  end

  def _graph()

    var top = 8
    var h = 7

    # Let the scale settle back down, slowly, so a one-off spike does not
    # flatten the next hour of graph.
    var seen = 0
    var i = 0
    while i < 52
      var v = self.hist[i]
      if v > seen
        seen = v
      end
      i += 1
    end
    if seen < 500
      seen = 500
    end
    if self.peak > seen
      self.peak = self.peak - (self.peak - seen) / 16 - 1
    end

    i = 0
    while i < 52
      var v = self.hist[(self.head + i) % 52]
      if v >= 0
        var bar = (v * h) / self.peak
        if bar < 1
          bar = 1
        end
        if bar > h
          bar = h
        end
        var shade = rgb(0, 90, 60)
        if v >= 3000
          shade = rgb(140, 30, 20)
        elif v >= 1000
          shade = rgb(140, 95, 0)
        end
        rect_fill(i, top + (h - bar), 1, bar, shade)
      end
      i += 1
    end
  end
end

return App()
