require 'bundler/setup'
require 'optparse'
require 'yaml'

require 'refinery/cli'

require_relative 'watch-and-control/meter'
require_relative 'watch-and-control/breaker'

# Version
Version = '0.1.0'


# Refinements
using Refinery::CLI::OptProcess
using Refinery::CLI::OptRuby
using Refinery::CLI::OptCommon


# Logger
$logger = Logger.new($stderr)


# Option parsing
$opts = { :config => 'config.yaml', :topic => 'water-breaker' }
op = OptionParser.new do |op|
    op.banner =  "Watch watermeter and control breaker."
    op.separator ""
    op.separator "Usage: #{op.program_name} [options]"
    op.separator ""

    op.on "-C", "--config",                 "Alternate configuration file"
    op.on "-t", "--topic",                  "MQTT topic (#{$opts[:topic]})"
    op.separator ""

    op.options_process
    op.options_ruby
    op.options_common
end
Process.setproctitle(op.program_name)
op.parse!(ARGV, into: $opts)

Refinery::CLI::OptProcess
    .process($opts, logger: $logger, progname: op.program_name)

Refinery::CLI::OptCommon
    .process($opts, logger: $logger)


# Load config
$cfg = YAML.load_file($opts[:config])


# Initializing
$topic  = [ $opts[:topic], $cfg[:name] ].join('/')
$mqtt   = MQTT::Client  .new($cfg.dig(:mqtt))
$m      = Moses::Meter  .new($mqtt, topic: $topic, logger: $logger)
$b      = Moses::Breaker.new($mqtt, topic: $topic, logger: $logger)



$meter_thr = Thread.new do
    loop do
        $m.loop(delay: $cfg.dig(:meter, :delay))
    end
end

$breaker_thr = Thread.new do
    loop do
        $b.loop
    end
end


[ $meter_thr, $breaker_thr ].each(&:join)

