(function (global) {
	"use strict"

	let has_module = false
	try { has_module = module !== undefined } catch {}
	if (has_module) {		
		let _ = require('lodash')
		let extensions = []
        let root_path = Root.GetDir('GameContent') + 'Scripts'
		
		function read_dir(dir) {
			let out = Root.ReadDirectory(dir)
			if (out.$) {				
				let items = _.filter(out.OutItems,(item) => !item.bIsDirectory && /^((?!node_modules).)*$/.test(item.Name) && /extension[^\.]*\.js$/.test(item.Name))
				extensions = extensions.concat(items.map((item) => item.Name.substr(root_path.length+1)))
				out.OutItems.forEach((item) => {
					if (item.bIsDirectory) {
						read_dir(item.Name)
					}
				})			
			}
		} 
	
		read_dir(root_path)
		
		function spawn(what) {
			try {
				return require(what)()			
			}
			catch (e) {
				console.error(String(e))
				return function () {}
			}
		}
		
		function main() {
		    let byes = _.filter(extensions.map((what) => spawn(what)),x => _.isFunction(x))

			return function () {
				byes.forEach((bye)=>bye())
			}
		}
		
		module.exports = () => {
			try {
				let cleanup = main()

				global.$$exit = cleanup
		
				return () => cleanup()
			} catch (e) {
				console.error(String(e))
				return () => {}
			}			
		}
	} else {
		global.$$exit = function() {}
		global.$exit = function () {
			global.$$exit()
		}
		Context.WriteDTS(Context.Paths[0] + 'typings/ue.d.ts')
		Context.WriteAliases(Context.Paths[0] + 'aliases.js')
	
		Context.RunFile('aliases.js')
		Context.RunFile('polyfill/unrealengine.js')
		Context.RunFile('polyfill/timers.js')
		
		require('devrequire')('editor')
	}	
})(this)
