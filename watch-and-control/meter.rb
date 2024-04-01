require 'rexml'
require 'shellwords'
require 'json'

require 'mqtt'

require 'refinery/mqtt'


module Moses
class Meter
    using Refinery::MQTTResilient

    TOPIC = 'meter'
    
    def initialize(mqtt, topic:, logger: nil)
        @mqtt   = mqtt
        @logger = logger
        @cmd    = [ '/opt/libmbus/bin/mbus-serial-request-data',
                    '-b', '2400',
                    '/dev/ttyAMA0',
                    '1'                               
                  ]
        @value_xpath = 'DataRecord[@id=1]/Value'
        @id_xpath    = 'SlaveInformation/Id'
        @topic       = "#{topic}/#{TOPIC}"
    end

    def process
        @logger&.debug { @cmd.map(&:shellescape).join(' ') }
        IO.popen(@cmd, :err => '/dev/null') do |io|
            xml   = REXML::Document.new(io)
            index = xml.root.elements[@value_xpath].text.to_i
            data  = { :index => index }
            @logger&.info "Index is #{index}"
            @mqtt&.publish(@topic, data.to_json)
        end
    end

    def loop(delay: 60)
        Kernel.loop do
            process
            sleep(delay)
        end
    end
end
end
