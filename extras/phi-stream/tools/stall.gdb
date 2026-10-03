set pagination off
set print thread-events off
set debuginfod enabled off
set confirm off
set $nw = 0
set $nd = 0
set $nf = 0
set $nfa = 0
break <phi_stream::engine::Engine>::write_status_file
commands
silent
set $nw = $nw + 1
continue
end
break <phi_stream::engine::Engine>::diag_text
commands
silent
set $nd = $nd + 1
continue
end
break <phi_stream::engine::Engine>::finish_agent_wait
commands
silent
set $nfa = $nfa + 1
continue
end
break std::fs::write::inner
commands
silent
set $nf = $nf + 1
printf "fs::write %d\n", $nf
bt 3
continue
end
