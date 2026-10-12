function doGet(e) {
  var cal = CalendarApp.getCalendarById('primary');
  var now = new Date();
  var end = new Date();
  end.setDate(now.getDate() + 10); 

  var events = cal.getEvents(now, end);
  var result = [];

  var palette = getPalette();

  var limit = Math.min(events.length, 8); 

  for (var i = 0; i < limit; i++) {
    var evt = events[i];
    var cId = evt.getColor();
    if (!cId) cId = "9"; 

    var hex = palette[cId] || palette["9"];
    
    var r = parseInt(hex.substring(1, 3), 16);
    var g = parseInt(hex.substring(3, 5), 16);
    var b = parseInt(hex.substring(5, 7), 16);
    var rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    var start = evt.getStartTime();
    var timeStr = start.toLocaleTimeString('en-US', {hour: '2-digit', minute:'2-digit', hour12: false});
    
    var dateStr = "";
    if (start.getDate() == now.getDate()) {
       dateStr = timeStr; 
    } else {
       dateStr = start.toLocaleDateString('en-US', {month: 'short', day: 'numeric'}) + " " + timeStr;
    }

    result.push({
      "t": evt.getTitle(),
      "d": dateStr,
      "c": rgb565,
      "isDark": isDarkColor(r, g, b)
    });
  }

  return ContentService.createTextOutput(JSON.stringify(result))
    .setMimeType(ContentService.MimeType.JSON);
}

// Fallback: classic Google Calendar event colours (ids 1-11)
var DEFAULT_PALETTE = {
  "1": "#7986cb", "2": "#33b679", "3": "#8e24aa", "4": "#e67c73",
  "5": "#f6c026", "6": "#f5511d", "7": "#039be5", "8": "#616161",
  "9": "#3f51b5", "10": "#0b8043", "11": "#d60000"
};

// Loads every event colour Google currently offers (including newer ones) via the
// Calendar advanced service (Services > + > Google Calendar API). Falls back to the
// classic 11 colours if the service isn't enabled.
function getPalette() {
  var palette = {};
  for (var k in DEFAULT_PALETTE) palette[k] = DEFAULT_PALETTE[k];
  try {
    var colors = Calendar.Colors.get().event;
    for (var id in colors) palette[id] = colors[id].background;
  } catch (err) {
    // advanced service not enabled - keep defaults
  }
  return palette;
}

// Perceived brightness decides white vs black text
function isDarkColor(r, g, b) {
  return (0.299 * r + 0.587 * g + 0.114 * b) < 150;
}
