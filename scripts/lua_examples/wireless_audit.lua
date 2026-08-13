aurora.print("Starting Wireless Security Monitor...")
aurora.wireless.set_channel(6)
aurora.wireless.start_monitor()

aurora.print("Listening for Deauth attacks...")
local alerts = aurora.wireless.get_alerts()
if #alerts == 0 then
    aurora.print("No attacks detected yet.")
else
    for i, alert in ipairs(alerts) do
        aurora.print("ALERT: " .. alert.type .. " from " .. alert.mac)
    end
end
aurora.wireless.stop_monitor()
