aurora.print("Starting automated port scan...")
aurora.scan.set_timeout(1500)
local target_ip = "192.168.1.100"
local ports = {22, 80, 443, 3306, 8080}

-- Initiate scan
aurora.scan.scan_tcp_port(target_ip, ports)

-- Wait for results (blocking wait for demo purposes)
aurora.print("Waiting for results...")
local wait_count = 0
while aurora.scan.result_count() < #ports and wait_count < 10 do
    -- Simple delay loop since we don't have a sleep binding yet
    for i=1,100000 do end 
    wait_count = wait_count + 1
end

aurora.print("Scan Complete! Results:")
while aurora.scan.has_results() do
    local ip, port, state, svc = aurora.scan.pop_result()
    aurora.print("Port " .. port .. " is " .. state)
end
