function doGet(e) {
  var cal = CalendarApp.getCalendarById('primary');
  var now = new Date();
  var end = new Date();
  end.setDate(now.getDate() + 10); 

  var events = cal.getEvents(now, end);
  var result = [];

  // OFFICIAL GOOGLE CALENDAR HEX CODES
  var palette = {
    "1": "#7986cb", // Lavender
    "2": "#33b679", // Sage
    "3": "#8e24aa", // Grape
    "4": "#e67c73", // Flamingo
    "5": "#f6c026", // Banana
    "6": "#f5511d", // Tangerine
    "7": "#039be5", // Peacock
    "8": "#616161", // Graphite
    "9": "#3f51b5", // Blueberry
    "10": "#0b8043", // Basil
    "11": "#d60000"  // Tomato
  };

  var limit = Math.min(events.length, 8); 

  for (var i = 0; i < limit; i++) {
    var evt = events[i];
    var cId = evt.getColor();
    if (!cId) cId = "9"; 

    var hex = palette[cId] || "#3f51b5";
    
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
      "isDark": (cId === "8" || cId === "9" || cId === "10" || cId === "11" || cId === "3") 
    });
  }

  return ContentService.createTextOutput(JSON.stringify(result))
    .setMimeType(ContentService.MimeType.JSON);
}