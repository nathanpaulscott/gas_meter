from datetime import datetime, timezone, timedelta

def epoch_to_gmt8(epoch_seconds):
    tz_gmt8 = timezone(timedelta(hours=8))
    dt = datetime.fromtimestamp(epoch_seconds, tz=tz_gmt8)
    return dt

# Example
epoch_value = 1764247293.17842
print(epoch_to_gmt8(epoch_value))