local pty = require "pty"

local ret  = {pty.ptyFork()}
local pid = ret[1]
if pid == -1 then
    print("error")
end

if pid == 0 then
    if #ret < 4 then
        print("errno: " .. ret[3])
        return
    end
    print(pty.exec("/bin/ls", {"-l"}))
    local stdin, stdout = ret[2], ret[3], ret[4]
    local epoll = require "cepoll"
    local ed = epoll.epoll_create()

    ed:epoll_ctladd(stdin:fd(), epoll.EPOLLIN, stdin)
    while true do
        local ret,res = ed:epoll_wait(10, 0)
        if ret and ret > 0 then
            for k, v in  ipairs(res) do
                if v.fd == stdin:fd() then
                    local msg, err= stdin:read(1024)
                    if msg and #msg == 0 then
                        --master closed
                        ed:epoll_ctldel(stdin:fd())
                        ed:epoll_close()
                    end
                    if msg then
                        stdout:write("from master: " .. msg)
                        if msg == "cend\n" then return end
                    end
                end
           end
        end
   end
end

if pid > 0 then
    if #ret < 5 then
        print("errno: " .. ret[3])
        return
    end
    local ms, stdin, stdout = ret[2], ret[3], ret[4]
    local epoll = require "cepoll"
    local ed = epoll.epoll_create()
    ed:setdebug(true)
    ed:epoll_ctladd(ms:fd(), epoll.EPOLLIN, ms)
    ed:epoll_ctladd(stdin:fd(), epoll.EPOLLIN, stdin)
    while true do
        local ret,res = ed:epoll_wait(10, 0)
	    if ret and ret > 0 then
	        for k, v in  ipairs(res) do
	            if v.fd == ms:fd() then
	                local out, err = ms:read(1024)
	                if out then
	                    --stdout:write("from slave: ")
	                    stdout:write(out)
	                else
	                    if err == 5 then
                            --slave closed
	                        ed:epoll_ctldel(ms:fd())
	                        ed:epoll_ctldel(stdin:fd())
                            ed:epoll_close()
                            return
	                    end
	                end
	           elseif v.fd == stdin:fd() then
	                local cmd, err = stdin:read(10)
	                if cmd then
	                    --if cmd == "p" then return end
	                    ms:write(cmd)
	                end
	           end
	       end
	   end
   end
end

