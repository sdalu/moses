require 'mqtt'
require 'refinery/mqtt'
require 'shellwords'

module Moses
class Breaker
    using Refinery::MQTTResilient

    TOPIC = 'breaker'

    def initialize(mqtt, topic:, logger: nil)
        @mqtt           = mqtt
        @topic          = "#{topic}/#{TOPIC}"
        @logger         = logger
        @gpio           = 16
        @state          = { :activated   => { :drive => 'dh', :value => true  },
                            :deactivated => { :drive => 'dl', :value => false } }
        @cmd_state_get  = [ 'raspi-gpio', 'get', @gpio.to_s ]
        @cmd_state_set  = [ 'raspi-gpio', 'set', @gpio.to_s ]
        @cmd_activate   = @cmd_state_set + [       @state[:activated  ][:drive] ]
        @cmd_deactivate = @cmd_state_set + [       @state[:deactivated][:drive] ]
        @cmd_config     = @cmd_state_set + [ 'op', @state[:deactivated][:drive] ]

        
        @logger&.debug { @cmd_config.map(&:shellescape).join(' ') }
        if (system(*@cmd_config) != true) || (state != :deactivated)
            @logger&.error "Configuring breaker failed"
            raise "configuring breaker failed"
        end
    end

    def activate
        if (system(*@cmd_activate) != true) || (state != :activated)
            @logger&.error "Activating breaker failed"
            return
        end
        @logger&.info "Breaker activated"        
        @mqtt.publish @topic, "activated"
    end

    def deactivate
        if (system(*@cmd_deactivate) != true) || (state != :deactivated)
            @logger&.error "Deactivating breaker failed"
            return
        end
        @logger&.info "Breaker deactivated"
        @mqtt.publish @topic, "deactivated"
    end

    def state
        IO.popen(@cmd_state_get, :err => '/dev/null') do |io|
            if io.read =~ /GPIO \d+: level=(\d) func=OUTPUT/
                ($1.to_i == 1).then {|v|
                    @state.find {|k,c| c[:value] == v }
                }[0]
            end
        end
    end
        
    def loop
        Kernel.loop do
            @mqtt.connect do |client|
                client.subscribe "#{@topic}/set"
                @logger&.info "Subscribed to topic: #{@topic}/set"
                client.get do |topic, message|
                    case message.downcase
                    when 'activate',   'activated',   'on',  'true',  '1'
                        activate
                    when 'deactivate', 'deactivated', 'off', 'false', '0'
                        deactivate
                    else @logger&.error "TOPIC[#{topic}]: #{message}"
                    end
                end
            end
            @logger&.error "MQTT connection ended (restart in 5s)"
            sleep(5)
        rescue MQTT::ProtocolException => e
            @logger&.error "MQTT connection ended [#{e}] (restart in 5s)"
            sleep(5)
        end
    end
end
end
